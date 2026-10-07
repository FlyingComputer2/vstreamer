#include "apps/common/pdu_stage.hpp"
#include "apps/common/queues.hpp"
#include "core/component.hpp"
#include "core/component_pdu.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace
{

class stub_component : public vstreamer::component
{
public:
    int configure(std::string_view, std::string_view) override { return -ENOTSUP; }
    int query(std::string_view, std::string *) const override { return -ENOTSUP; }
};

}  // namespace

TEST(StageQueueWakeTest, PopWaitsOnQueueWakeupNotFixedDelay)
{
    std::atomic<bool> run {true};
    vstreamer::apps::pipeline_pdu_queue q(4, run);
    const auto wake = std::make_shared<vstreamer::pdu_wakeup>();
    q.bind_wakeup(wake);
    stub_component owner;

    std::mutex              mu;
    std::condition_variable cv;
    bool                    consumer_waiting = false;
    double                  blocked_ms = 0.0;

    std::thread consumer([&]() {
        vstreamer::component_pdu pdu;
        {
            std::lock_guard<std::mutex> lock(mu);
            consumer_waiting = true;
        }
        cv.notify_one();
        const auto t0 = std::chrono::steady_clock::now();
        while (run.load(std::memory_order_relaxed) && !q.try_pop(pdu))
        {
            vstreamer::apps::wait_for_pdu(*wake, owner, run);
        }
        ASSERT_TRUE(run.load(std::memory_order_relaxed));
        const auto t1 = std::chrono::steady_clock::now();
        blocked_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
    });

    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return consumer_waiting; });
    }
    vstreamer::component_pdu pdu;
    pdu.seq = 1;
    ASSERT_TRUE(q.push(std::move(pdu)));

    consumer.join();
    run = false;
    q.wake_shutdown();

    EXPECT_LT(blocked_ms, 10.0) << "queue pop should wake on push, not a fixed 50ms stage wait";
}

TEST(StageQueueWakeTest, EnqueueMonoSurvivesOverflowDrops)
{
    std::atomic<bool> run {true};
    vstreamer::apps::pipeline_pdu_queue q(2, run);
    stub_component owner;
    vstreamer::pdu_wakeup wake;

    for (int i = 0; i < 10'000; ++i)
    {
        vstreamer::component_pdu pdu;
        pdu.seq = static_cast<uint64_t>(i);
        pdu.sdu_type = vstreamer::sdu_type_e::NV12;
        ASSERT_TRUE(q.push(std::move(pdu)));
    }

    int64_t mono = 0;
    for (int i = 0; i < 2; ++i)
    {
        vstreamer::component_pdu pdu;
        ASSERT_TRUE(q.try_pop(pdu, &mono));
        EXPECT_GT(mono, 0);
    }
    run = false;
    q.wake_shutdown();
}
