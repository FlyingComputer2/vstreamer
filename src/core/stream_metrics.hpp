#ifndef VSTREAMER_CORE_STREAM_METRICS_HPP
#define VSTREAMER_CORE_STREAM_METRICS_HPP

#include <charconv>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace vstreamer
{

struct stream_metric_tuple
{
    uint64_t timestamp_ms = 0;
    uint64_t first = 0;
    uint64_t second = 0;
};

inline uint64_t stream_metric_timestamp_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline std::string stream_metric_format(uint64_t ts, uint64_t first,
                                        uint64_t second)
{
    return std::to_string(ts) + "," + std::to_string(first) + "," +
           std::to_string(second);
}

inline bool stream_metric_parse(std::string_view text, stream_metric_tuple *out)
{
    if (nullptr == out)
    {
        return false;
    }
    stream_metric_tuple tuple;
    uint64_t *fields[] = {&tuple.timestamp_ms, &tuple.first, &tuple.second};
    for (size_t i = 0; i < 3; ++i)
    {
        const size_t comma = text.find(',');
        if ((i < 2 && comma == std::string_view::npos) ||
            (i == 2 && comma != std::string_view::npos))
        {
            return false;
        }
        const auto part = text.substr(0, comma);
        if (part.empty())
        {
            return false;
        }
        const auto result =
            std::from_chars(part.data(), part.data() + part.size(), *fields[i]);
        if (result.ec != std::errc{} || result.ptr != part.data() + part.size())
        {
            return false;
        }
        if (i < 2)
        {
            text.remove_prefix(comma + 1);
        }
    }
    *out = tuple;
    return true;
}

template <typename Component>
inline bool query_stream_metric(const Component &component,
                                std::string_view key, stream_metric_tuple *out)
{
    std::string value;
    return component.query(key, &value) == 0 && stream_metric_parse(value, out);
}

template <typename Component>
inline bool query_stream_u64(const Component &component, std::string_view key,
                             uint64_t *out)
{
    std::string value;
    if (nullptr == out || component.query(key, &value) != 0 || value.empty())
    {
        return false;
    }
    uint64_t n = 0;
    const auto r =
        std::from_chars(value.data(), value.data() + value.size(), n);
    if (r.ec != std::errc{} || r.ptr != value.data() + value.size())
    {
        return false;
    }
    *out = n;
    return true;
}

// Keep the last valid baseline across query failures. Repeated timestamps do
// not advance it; counter rollback or a changed session starts a fresh rate
// window.
struct stream_metric_rate
{
    stream_metric_tuple previous{};
    uint64_t session = 0;
    bool have = false;

    bool sample(const stream_metric_tuple &current, uint64_t current_session,
                double *first_rate, double *second_rate)
    {
        if (!have || session != current_session ||
            current.timestamp_ms < previous.timestamp_ms ||
            current.first < previous.first || current.second < previous.second)
        {
            previous = current;
            session = current_session;
            have = true;
            *first_rate = *second_rate = 0.;
            return true;
        }
        if (current.timestamp_ms == previous.timestamp_ms)
        {
            return false;
        }
        const double sec =
            static_cast<double>(current.timestamp_ms - previous.timestamp_ms) /
            1000.;
        *first_rate = static_cast<double>(current.first - previous.first) / sec;
        *second_rate =
            static_cast<double>(current.second - previous.second) / sec;
        previous = current;
        return true;
    }
};

}  // namespace vstreamer
#endif
