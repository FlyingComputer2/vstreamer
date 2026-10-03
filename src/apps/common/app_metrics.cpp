#include "apps/common/app_metrics.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace vstreamer::apps
{

void format_stats_timestamp(char *buf, size_t buflen)
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t sec = clock::to_time_t(now);
    std::tm           tm_local {};
#if defined(_WIN32)
    localtime_s(&tm_local, &sec);
#else
    localtime_r(&sec, &tm_local);
#endif
    std::snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm_local.tm_year + 1900,
                  tm_local.tm_mon + 1, tm_local.tm_mday, tm_local.tm_hour, tm_local.tm_min,
                  tm_local.tm_sec, static_cast<int>(ms.count()));
}

[[nodiscard]] double elapsed_sec(std::chrono::steady_clock::time_point t0,
                                 std::chrono::steady_clock::time_point t1)
{
    const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0);
    return dt.count() > 0.0 ? dt.count() : 1.0;
}

[[nodiscard]] double rate_per_sec(uint64_t now, uint64_t prev, double dt_sec)
{
    if (now <= prev)
    {
        return 0.0;
    }
    return static_cast<double>(now - prev) / dt_sec;
}

[[nodiscard]] uint64_t parse_stats_field(std::string_view stats, const char *key)
{
    const std::string prefix = std::string(key) + "=";
    const auto              pos = stats.find(prefix);
    if (pos == std::string_view::npos)
    {
        return 0;
    }
    const char *start = stats.data() + pos + prefix.size();
    char       *end = nullptr;
    return std::strtoull(start, &end, 10);
}

}  // namespace vstreamer::apps
