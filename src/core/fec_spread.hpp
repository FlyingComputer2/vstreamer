#ifndef VSTREAMER_CORE_FEC_SPREAD_HPP
#define VSTREAMER_CORE_FEC_SPREAD_HPP

#include <chrono>
#include <cstddef>

namespace vstreamer
{

/*
 * Release offset of shard j (in emit order: data, then parity) of a block of n shards
 * spread evenly over spread_ms: j * spread_ms / n. The first shard goes at once and the last
 * one spread_ms / n before the window ends, so back-to-back blocks tile the time line.
 * spread_ms <= 0 or n == 0 gives 0 (send at once).
 */
[[nodiscard]] inline std::chrono::microseconds fec_spread_offset(size_t j, size_t n,
                                                                 int spread_ms)
{
    if (spread_ms <= 0 || 0 == n)
    {
        return std::chrono::microseconds(0);
    }
    const long long window_us = static_cast<long long>(spread_ms) * 1000;
    return std::chrono::microseconds(window_us * static_cast<long long>(j) /
                                     static_cast<long long>(n));
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_FEC_SPREAD_HPP
