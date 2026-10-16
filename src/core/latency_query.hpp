#ifndef VSTREAMER_CORE_LATENCY_QUERY_HPP
#define VSTREAMER_CORE_LATENCY_QUERY_HPP

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace vstreamer
{

/*
 * The standard component latency queries (docs/vstreamer.md, "Latency metrics"):
 *   latency_ms       capture age (now - ts_us) of the last frame out of the component
 *   node_latency_ms  exit - entry on the same frame, for the last frame through the component
 * Both hold -1 until a frame is measured; the query then answers -ENODATA.
 */
inline void latency_store(std::atomic<double> &slot, int64_t now_ns, int64_t then_ns)
{
    if (then_ns > 0 && now_ns >= then_ns)
    {
        slot.store(static_cast<double>(now_ns - then_ns) / 1e6, std::memory_order_relaxed);
    }
}

/* -ENOTSUP for any other key, so callers can fall through to their own. */
inline int latency_query(std::string_view key, const std::atomic<double> &latency_ms,
                         const std::atomic<double> &node_latency_ms, std::string *value)
{
    const std::atomic<double> *slot = nullptr;
    if (key == "latency_ms")
    {
        slot = &latency_ms;
    }
    else if (key == "node_latency_ms")
    {
        slot = &node_latency_ms;
    }
    else
    {
        return -ENOTSUP;
    }
    const double ms = slot->load(std::memory_order_relaxed);
    if (ms < 0.0)
    {
        return -ENODATA;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", ms);
    *value = buf;
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_LATENCY_QUERY_HPP
