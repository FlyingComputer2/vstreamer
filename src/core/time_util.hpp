#ifndef VSTREAMER_CORE_TIME_UTIL_HPP
#define VSTREAMER_CORE_TIME_UTIL_HPP

#include <chrono>
#include <cstdint>
#include <ctime>

namespace vstreamer
{

static_assert(std::chrono::steady_clock::is_steady,
              "steady_clock must be CLOCK_MONOTONIC on this platform");

/* Monotonic ns from steady_clock (same epoch on one process; use for latency deltas). */
[[nodiscard]] inline int64_t steady_mono_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* CLOCK_REALTIME ns since Unix epoch. */
[[nodiscard]] inline int64_t realtime_ns()
{
    timespec ts {};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    {
        return 0;
    }
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL +
           static_cast<int64_t>(ts.tv_nsec);
}

/* Map local monotonic capture time to realtime using a fresh paired sample. */
[[nodiscard]] inline int64_t mono_to_realtime_ns(int64_t mono_ns)
{
    if (mono_ns <= 0)
    {
        return 0;
    }
    const int64_t mono = steady_mono_ns();
    const int64_t rt = realtime_ns();
    const int64_t offset = rt - mono;
    return mono_ns + offset;
}

/* Map wire realtime capture time to local monotonic using a fresh paired sample. */
[[nodiscard]] inline int64_t realtime_to_mono_ns(int64_t rt_ns)
{
    if (rt_ns <= 0)
    {
        return 0;
    }
    const int64_t mono = steady_mono_ns();
    const int64_t rt = realtime_ns();
    const int64_t offset = rt - mono;
    return rt_ns - offset;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_TIME_UTIL_HPP
