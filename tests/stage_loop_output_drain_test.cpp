#include "apps/common/pdu_stage.hpp"
#include "apps/common/queues.hpp"
#include "core/component.hpp"
#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/component_pdu.hpp"
#include "core/pdu_wakeup.hpp"
#include "core/sdu_type.hpp"
#include "core/time_util.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace
{

class delayed_nv12_stage : public vstreamer::component,
                            public vstreamer::component_input,
                            public vstreamer::component_output
{
public:
    int configure(std::string_view, std::string_view) override { return -ENOTSUP; }
    int query(std::string_view, std::string *) const override { return -ENOTSUP; }

    int input(vstreamer::component_pdu &&in) override
    {
        if (in.sdu_type != vstreamer::sdu_type_e::MJPEG)
        {
            return -EINVAL;
        }
        const uint64_t ts = in.ts_us;
        const uint64_t seq = in.seq;
        std::thread([this, ts, seq]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            vstreamer::component_pdu out;
            out.sdu_type = vstreamer::sdu_type_e::NV12;
            out.ts_us = ts;
            out.seq = seq;
            out.sdu.resize(16);
            {
                std::lock_guard<std::mutex> lock(mu_);
                ready_.push_back(std::move(out));
            }
            if (wake_)
            {
                wake_->notify();
            }
        }).detach();
        return 0;
    }

    int output(vstreamer::component_pdu &out) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (ready_.empty())
        {
            return -EAGAIN;
        }
        out = std::move(ready_.front());
        ready_.pop_front();
        return 0;
    }

    void bind_wake(std::shared_ptr<vstreamer::pdu_wakeup> w) { wake_ = std::move(w); }

private:
    mutable std::mutex                   mu_;
    std::deque<vstreamer::component_pdu> ready_;
    std::shared_ptr<vstreamer::pdu_wakeup> wake_;
};

void run_stage_loop(vstreamer::apps::pipeline_pdu_queue &in_q, delayed_nv12_stage &stage,
                    std::atomic<bool> &run, double *max_output_lag_ms)
{
    auto *stage_in = dynamic_cast<vstreamer::component_input *>(&stage);
    auto *stage_out = dynamic_cast<vstreamer::component_output *>(&stage);
    auto *owner = dynamic_cast<vstreamer::component *>(&stage);
    ASSERT_NE(nullptr, stage_in);
    ASSERT_NE(nullptr, stage_out);
    ASSERT_NE(nullptr, owner);

    std::shared_ptr<vstreamer::pdu_wakeup> wake = in_q.shared_wakeup();
    if (!wake)
    {
        wake = std::make_shared<vstreamer::pdu_wakeup>();
        in_q.bind_wakeup(wake);
    }
    stage.bind_wake(wake);
    owner->set_wakeup(wake);

    vstreamer::component_pdu raw;
    bool                     holding = false;
    int                      inflight = 0;
    const int                max_inflight = 2;
    const auto               t_start = std::chrono::steady_clock::now();

    while (run.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() - t_start < std::chrono::seconds(2))
    {
        bool progressed = false;
        vstreamer::component_pdu out;
        for (;;)
        {
            const int or_out = stage_out->output(out);
            if (0 == or_out)
            {
                progressed = true;
                const int64_t now_ns = vstreamer::steady_mono_ns();
                const int64_t cap_ns = static_cast<int64_t>(out.ts_us) * 1000LL;
                const double  lag_ms = static_cast<double>(now_ns - cap_ns) / 1e6;
                if (nullptr != max_output_lag_ms)
                {
                    *max_output_lag_ms = std::max(*max_output_lag_ms, lag_ms);
                }
                inflight--;
                continue;
            }
            if (-EAGAIN == or_out)
            {
                break;
            }
            return;
        }
        while (inflight < max_inflight)
        {
            if (!holding)
            {
                if (!in_q.try_pop(raw))
                {
                    break;
                }
                holding = true;
                progressed = true;
            }
            const int ir = stage_in->input(std::move(raw));
            if (0 == ir)
            {
                holding = false;
                inflight++;
                continue;
            }
            if (-EAGAIN == ir)
            {
                break;
            }
            holding = false;
            break;
        }
        if (!progressed)
        {
            vstreamer::apps::wait_for_pdu(*wake, *owner, run);
        }
    }
}

}  // namespace

TEST(StageLoopDrainTest, ComponentOutputLeavesWithin2msOfReady)
{
    std::atomic<bool> run {true};
    vstreamer::apps::pipeline_pdu_queue in_q(8, run);
    delayed_nv12_stage                    stage;

    double max_lag_ms = 0.0;
    std::thread feeder([&]() {
        uint64_t seq = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (run.load(std::memory_order_relaxed) &&
               std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(900))
        {
            vstreamer::component_pdu pdu;
            pdu.sdu_type = vstreamer::sdu_type_e::MJPEG;
            pdu.ts_us = static_cast<uint64_t>(vstreamer::steady_mono_ns() / 1000LL);
            pdu.seq = ++seq;
            pdu.sdu.resize(8);
            (void)in_q.push(std::move(pdu));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        run = false;
        in_q.wake_shutdown();
    });

    std::thread consumer([&]() { run_stage_loop(in_q, stage, run, &max_lag_ms); });

    feeder.join();
    consumer.join();

    EXPECT_GT(max_lag_ms, 0.0);
    EXPECT_LT(max_lag_ms, 15.0)
        << "decoded output should leave soon after ready, not wait for next input (~28ms)";
}
