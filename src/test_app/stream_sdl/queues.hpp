#ifndef VSTREAMER_TEST_APP_QUEUES_HPP
#define VSTREAMER_TEST_APP_QUEUES_HPP

#include "test_app/stream_sdl/diag.hpp"
#include "test_app/stream_sdl/pipeline_state.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>

#include "core/data_packet.hpp"

namespace vstreamer::test_app
{

using vstreamer::data_packet;

constexpr size_t k_default_pipe_queue_depth = 8;
constexpr size_t k_default_present_queue_depth = 1;
constexpr size_t k_default_rx_au_queue_depth = 4;

[[nodiscard]] inline size_t queue_depth_from_env(const char *name, size_t default_val,
                                                 size_t max_val)
{
    const char *v = std::getenv(name);
    if (nullptr == v || v[0] == '\0')
    {
        return default_val;
    }
    char *end = nullptr;
    const unsigned long n = std::strtoul(v, &end, 10);
    if (end == v || n == 0)
    {
        return default_val;
    }
    return static_cast<size_t>(std::min(n, static_cast<unsigned long>(max_val)));
}

class pipeline_queue
{
public:
    explicit pipeline_queue(size_t cap) : capacity(cap) {}

    bool push(data_packet pkt, std::atomic<uint64_t> *drops = nullptr)
    {
        std::unique_lock<std::mutex> lock(mu);
        if (!g_run.load())
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

    /* Block until there is capacity (or shutdown). Avoids dropping NV12 before encode. */
    bool push_wait(data_packet pkt, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto room = [this] { return !g_run.load() || q.size() < capacity; };
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
        if (!g_run.load())
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
        const auto ready = [this] { return !g_run.load() || !q.empty(); };
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
    size_t                    capacity;
    std::mutex                mu;
    std::condition_variable   cv_push;
    std::condition_variable   cv_pop;
    std::deque<data_packet>   q;
};

/* Decoded NV12 waiting for SDL (RX enqueues; present thread draws). */
class present_frame_queue
{
public:
    explicit present_frame_queue(size_t cap) : capacity(cap) {}

    bool push(data_packet pkt, std::atomic<uint64_t> *drops = nullptr)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!g_run.load())
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
        const auto ready = [this] { return !g_run.load() || !q.empty(); };
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
    std::mutex              mu;
    std::condition_variable cv_pop;
    std::deque<data_packet> q;
};

struct rx_au_queue
{
    explicit rx_au_queue(size_t cap) : capacity(cap) {}

    size_t capacity = k_default_rx_au_queue_depth;

    void push(data_packet pkt, bench_diag &diag)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!g_run.load())
        {
            return;
        }
        if (q.size() >= capacity)
        {
            q.pop_front();
            diag.rx_au_q_drop++;
        }
        q.push_back(std::move(pkt));
        cv.notify_one();
    }

    bool pop(data_packet &out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu);
        const auto                   ready = [this] { return !g_run.load() || !q.empty(); };
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
};

}  // namespace vstreamer::test_app

#endif
