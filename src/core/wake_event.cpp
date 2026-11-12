#include "core/wake_event.hpp"

#include <chrono>
#include <climits>

#include "core/time_util.hpp"

namespace vstreamer
{

void wake_event::notify()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_ = true;
    }
    cv_.notify_one();
}

void wake_event::wait_until(int64_t deadline_mono_ns)
{
    std::unique_lock<std::mutex> lock(mu_);
    if (deadline_mono_ns == INT64_MAX)
    {
        cv_.wait(lock, [this]() { return pending_; });
    }
    else
    {
        const auto deadline = std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(deadline_mono_ns));
        cv_.wait_until(lock, deadline, [this]() { return pending_; });
    }
    pending_ = false;
}

}  // namespace vstreamer
