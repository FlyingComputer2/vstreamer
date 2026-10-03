#ifndef VSTREAMER_APPS_APP_METRICS_HPP
#define VSTREAMER_APPS_APP_METRICS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vstreamer::apps
{

void format_stats_timestamp(char *buf, size_t buflen);

[[nodiscard]] double elapsed_sec(std::chrono::steady_clock::time_point t0,
                                 std::chrono::steady_clock::time_point t1);

[[nodiscard]] double rate_per_sec(uint64_t now, uint64_t prev, double dt_sec);

[[nodiscard]] uint64_t parse_stats_field(std::string_view stats, const char *key);

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_APP_METRICS_HPP
