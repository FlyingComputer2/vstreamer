#ifndef VSTREAMER_APPS_QUEUES_HPP
#define VSTREAMER_APPS_QUEUES_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

#include "core/data_packet.hpp"

namespace vstreamer::apps
{

using vstreamer::data_packet;

constexpr size_t k_default_pipe_queue_depth = 8;
constexpr size_t k_default_present_queue_depth = 1;
constexpr size_t k_default_rx_au_queue_depth = 4;

[[nodiscard]] size_t queue_depth_from_env(const char *name, size_t default_val, size_t max_val);

class pipeline_queue
{
public:
    pipeline_queue(size_t cap, std::atomic<bool> &run) : capacity(cap), run_(run) {}

    bool push(data_packet pkt, std::atomic<uint64_t> *drops = nullptr)
    {
        std::unique_lock<std::mutex> lock(mu);
        if (!run_.load())
        {
            return false;
        }
        if (q.size() >= capacity)
        {
            q.pop_front();
            if (nullptr != drops)
            {
                drops->fetch_add(1);
            }
        }
        q.push_back(std::move(pkt));
        cv_pop.notify_one();
        return true;
    }

    bool push_wait(data_packet pkt, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto room = [this] { return !run_.load() || q.size() < capacity; };
        if (timeout_ms < 0)
        {
            cv_push.wait(lock, room);
        }
        else if (timeout_ms > 0)
        {
            if (!cv_push.wait_for(lock, std::chrono::milliseconds(timeout_ms), room))
            {
                return false;
            }
        }
        else if (!room())
        {
            return false;
        }
        if (!run_.load())
        {
            return false;
        }
        q.push_back(std::move(pkt));
        cv_pop.notify_one();
        return true;
    }

    [[nodiscard]] size_t size()
    {
        std::lock_guard<std::mutex> lock(mu);
        return q.size();
    }

    bool pop(data_packet &out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto ready = [this] { return !run_.load() || !q.empty(); };
        if (timeout_ms < 0)
        {
            cv_pop.wait(lock, ready);
        }
        else if (timeout_ms > 0)
        {
            cv_pop.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
        else if (!ready())
        {
            return false;
        }
        if (q.empty())
        {
            return false;
        }
        out = std::move(q.front());
        q.pop_front();
        cv_push.notify_all();
        return true;
    }

    void wake_shutdown()
    {
        cv_push.notify_all();
        cv_pop.notify_all();
    }

private:
    size_t                  capacity;
    std::atomic<bool>      &run_;
    std::mutex              mu;
    std::condition_variable cv_push;
    std::condition_variable cv_pop;
    std::deque<data_packet> q;
};

class present_frame_queue
{
public:
    present_frame_queue(size_t cap, std::atomic<bool> &run) : capacity(cap), run_(run) {}

    bool push(data_packet pkt, std::atomic<uint64_t> *drops = nullptr)
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
        return true;
    }

    bool pop(data_packet &out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto ready = [this] { return !run_.load() || !q.empty(); };
        if (timeout_ms < 0)
        {
            cv_pop.wait(lock, ready);
        }
        else if (timeout_ms > 0)
        {
            cv_pop.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
        else if (!ready())
        {
            return false;
        }
        if (q.empty())
        {
            return false;
        }
        out = std::move(q.front());
        q.pop_front();
        return true;
    }

    void wake()
    {
        cv_pop.notify_all();
    }

private:
    size_t                  capacity;
    std::atomic<bool>      &run_;
    std::mutex              mu;
    std::condition_variable cv_pop;
    std::deque<data_packet> q;
};

struct rx_au_queue
{
    rx_au_queue(size_t cap, std::atomic<bool> &run) : capacity(cap), run_(run) {}

    size_t capacity = k_default_rx_au_queue_depth;

    void push(data_packet pkt, std::atomic<uint64_t> *au_q_drop_counter = nullptr)
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
    }

    bool pop(data_packet &out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto                   ready = [this] { return !run_.load() || !q.empty(); };
        if (timeout_ms < 0)
        {
            cv.wait(lock, ready);
        }
        else if (timeout_ms > 0)
        {
            cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
        else if (!ready())
        {
            return false;
        }
        if (q.empty())
        {
            return false;
        }
        out = std::move(q.front());
        q.pop_front();
        return true;
    }

    void wake()
    {
        cv.notify_all();
    }

    std::mutex              mu;
    std::condition_variable cv;
    std::deque<data_packet> q;
    std::atomic<bool>      &run_;
};

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_QUEUES_HPP
