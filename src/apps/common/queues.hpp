#ifndef VSTREAMER_APPS_QUEUES_HPP
#define VSTREAMER_APPS_QUEUES_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <memory>
#include <unordered_map>

#include "apps/common/pdu_stage.hpp"
#include "core/component.hpp"
#include "core/component_pdu.hpp"
#include "core/pdu_wakeup.hpp"
#include "core/sdu_caps.hpp"
#include "core/time_util.hpp"

namespace vstreamer::apps
{

constexpr size_t k_default_pipe_queue_depth = 8;
constexpr size_t k_default_present_queue_depth = 1;
constexpr size_t k_default_rx_au_queue_depth = 4;

[[nodiscard]] size_t queue_depth_from_env(const char *name, size_t default_val, size_t max_val);

class pipeline_pdu_queue
{
public:
    pipeline_pdu_queue(size_t cap, std::atomic<bool> &run) : capacity(cap), run_(run) {}

    void bind_wakeup(std::shared_ptr<pdu_wakeup> w) { wake_ = std::move(w); }

    [[nodiscard]] std::shared_ptr<pdu_wakeup> shared_wakeup() const { return wake_; }

    void bind_enqueue_mono_tracker(std::unordered_map<uint64_t, int64_t> *tracker)
    {
        enqueue_mono_by_ts_us_ = tracker;
    }

    bool push(component_pdu pkt, std::atomic<uint64_t> *drops = nullptr)
    {
        std::unique_lock<std::mutex> lock(mu);
        if (!run_.load())
        {
            return false;
        }
        if (nullptr != enqueue_mono_by_ts_us_ && !is_caps(pkt.sdu_type) && pkt.ts_us > 0)
        {
            note_pdu_input_ts(pkt.ts_us, steady_mono_ns(), enqueue_mono_by_ts_us_);
        }
        if (q.size() >= capacity)
        {
            q.pop_front();
            if (nullptr != drops)
            {
                drops->fetch_add(1, std::memory_order_relaxed);
            }
        }
        q.push_back(std::move(pkt));
        cv_pop.notify_one();
        if (wake_)
        {
            wake_->notify();
        }
        return true;
    }

    [[nodiscard]] size_t size()
    {
        std::lock_guard<std::mutex> lock(mu);
        return q.size();
    }

    bool pop(component_pdu &out, pdu_wakeup &w, component &deadline_owner)
    {
        while (run_.load(std::memory_order_relaxed))
        {
            {
                std::unique_lock<std::mutex> lock(mu);
                if (!q.empty())
                {
                    out = std::move(q.front());
                    q.pop_front();
                    return true;
                }
            }
            wait_for_pdu(w, deadline_owner, run_);
        }
        return false;
    }

    void wake_shutdown()
    {
        cv_pop.notify_all();
        if (wake_)
        {
            wake_->notify();
        }
    }

private:
    size_t                       capacity;
    std::atomic<bool>           &run_;
    std::mutex                   mu;
    std::condition_variable      cv_pop;
    std::deque<component_pdu>    q;
    std::shared_ptr<pdu_wakeup>  wake_;
    std::unordered_map<uint64_t, int64_t> *enqueue_mono_by_ts_us_ = nullptr;
};

class present_pdu_queue
{
public:
    present_pdu_queue(size_t cap, std::atomic<bool> &run) : capacity(cap), run_(run) {}

    void bind_wakeup(std::shared_ptr<pdu_wakeup> w) { wake_ = std::move(w); }

    [[nodiscard]] std::shared_ptr<pdu_wakeup> shared_wakeup() const { return wake_; }

    bool push(component_pdu pkt, std::atomic<uint64_t> *drops = nullptr)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!run_.load())
        {
            return false;
        }
        if (q.size() >= capacity)
        {
            q.pop_front();
            if (nullptr != drops)
            {
                (*drops)++;
            }
        }
        q.push_back(std::move(pkt));
        cv_pop.notify_one();
        if (wake_)
        {
            wake_->notify();
        }
        return true;
    }

    bool pop(component_pdu &out, pdu_wakeup &w, component &deadline_owner)
    {
        while (run_.load(std::memory_order_relaxed))
        {
            {
                std::unique_lock<std::mutex> lock(mu);
                if (!q.empty())
                {
                    out = std::move(q.front());
                    q.pop_front();
                    return true;
                }
            }
            wait_for_pdu(w, deadline_owner, run_);
        }
        return false;
    }

    void wake()
    {
        cv_pop.notify_all();
        if (wake_)
        {
            wake_->notify();
        }
    }

private:
    size_t                       capacity;
    std::atomic<bool>           &run_;
    std::mutex                   mu;
    std::condition_variable      cv_pop;
    std::deque<component_pdu>    q;
    std::shared_ptr<pdu_wakeup>  wake_;
};

struct pdu_rx_au_queue
{
    pdu_rx_au_queue(size_t cap, std::atomic<bool> &run) : capacity(cap), run_(run) {}

    size_t capacity = k_default_rx_au_queue_depth;

    void bind_wakeup(std::shared_ptr<pdu_wakeup> w) { wake_ = std::move(w); }

    [[nodiscard]] std::shared_ptr<pdu_wakeup> shared_wakeup() const { return wake_; }

    void push(component_pdu pkt, std::atomic<uint64_t> *au_q_drop_counter = nullptr)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!run_.load())
        {
            return;
        }
        if (q.size() >= capacity)
        {
            q.pop_front();
            if (nullptr != au_q_drop_counter)
            {
                au_q_drop_counter->fetch_add(1, std::memory_order_relaxed);
            }
        }
        q.push_back(std::move(pkt));
        cv.notify_one();
        if (wake_)
        {
            wake_->notify();
        }
    }

    bool pop(component_pdu &out, pdu_wakeup &w, component &deadline_owner)
    {
        while (run_.load(std::memory_order_relaxed))
        {
            {
                std::unique_lock<std::mutex> lock(mu);
                if (!q.empty())
                {
                    out = std::move(q.front());
                    q.pop_front();
                    return true;
                }
            }
            wait_for_pdu(w, deadline_owner, run_);
        }
        return false;
    }

    void wake()
    {
        cv.notify_all();
        if (wake_)
        {
            wake_->notify();
        }
    }

    std::mutex                   mu;
    std::condition_variable      cv;
    std::deque<component_pdu>    q;
    std::atomic<bool>           &run_;
    std::shared_ptr<pdu_wakeup>  wake_;
};

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_QUEUES_HPP
