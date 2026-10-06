#include "core/metrics.hpp"

#include <cinttypes>
#include <cstdio>

namespace vstreamer
{

void metric_store(metric &m, uint64_t v)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    m.value = v;
}

void metric_store(metric &m, const std::atomic<uint64_t> &counter)
{
    metric_store(m, counter.load(std::memory_order_relaxed));
}

void metric_store(metric &m, int64_t v)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    m.value = v;
}

void metric_store(metric &m, double v)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    m.value = v;
}

void metric_store(metric &m, const std::string &v)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    m.value = v;
}

void metric_store(metric &m, const char *v)
{
    metric_store(m, std::string(v != nullptr ? v : ""));
}

metrics::metrics() = default;

metrics::~metrics() = default;

std::shared_ptr<metric> metrics::get_metric(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mu);
    const auto it = index.find(name);
    if (it != index.end())
    {
        return entries[it->second].second;
    }

    auto ptr = std::make_shared<metric>();
    ptr->value = uint64_t{0};
    const size_t pos = entries.size();
    entries.emplace_back(name, ptr);
    index.emplace(name, pos);
    return ptr;
}

namespace
{

[[nodiscard]] std::string strip_trailing_angle_comment(std::string s)
{
    const auto pos = s.rfind(" <");
    if (pos != std::string::npos)
    {
        s.erase(pos);
    }
    return s;
}

[[nodiscard]] std::string trim_trailing_fraction_zeros(std::string s)
{
    const auto dot = s.find('.');
    if (dot == std::string::npos)
    {
        return s;
    }
    while (s.size() > dot + 1 && s.back() == '0')
    {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.')
    {
        s.pop_back();
    }
    return s;
}

[[nodiscard]] std::string format_double_metric(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return trim_trailing_fraction_zeros(buf);
}

[[nodiscard]] std::string format_metric_value(metric &m, const std::string &key)
{
    std::lock_guard<std::mutex> lock(m.mutex);
    if (std::holds_alternative<uint64_t>(m.value))
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, std::get<uint64_t>(m.value));
        return buf;
    }
    if (std::holds_alternative<int64_t>(m.value))
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRId64, std::get<int64_t>(m.value));
        return buf;
    }
    if (std::holds_alternative<double>(m.value))
    {
        const double v = std::get<double>(m.value);
        char         buf[48];
        if (key.size() >= 3 && key.compare(key.size() - 3, 3, "_ms") == 0)
        {
            std::snprintf(buf, sizeof(buf), "%.1f", v);
            return buf;
        }
        if (key.size() >= 4 && key.compare(key.size() - 4, 4, "_pct") == 0)
        {
            std::snprintf(buf, sizeof(buf), "%.2f", v);
            return buf;
        }
        return format_double_metric(v);
    }
    return strip_trailing_angle_comment(std::get<std::string>(m.value));
}

[[nodiscard]] bool split_section_key(const std::string &name, std::string &section, std::string &key)
{
    const auto dot = name.find('.');
    if (dot == std::string::npos)
    {
        section = name;
        key.clear();
        return false;
    }
    section = name.substr(0, dot);
    key = name.substr(dot + 1);
    return !key.empty();
}

}  // namespace

bool metrics::format_metric(const std::string &name, std::string *out) const
{
    if (nullptr == out)
    {
        return false;
    }
    std::shared_ptr<metric> ptr;
    {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto &entry : entries)
        {
            if (entry.first == name)
            {
                ptr = entry.second;
                break;
            }
        }
    }
    if (nullptr == ptr)
    {
        return false;
    }
    std::string section;
    std::string key;
    if (!split_section_key(name, section, key))
    {
        key = name;
    }
    *out = format_metric_value(*ptr, key);
    return true;
}

std::string metrics::to_string() const
{
    return to_string([](std::string_view) { return true; });
}

std::string metrics::to_string(metric_name_filter filter) const
{
    std::vector<std::pair<std::string, std::shared_ptr<metric>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mu);
        snapshot = entries;
    }

    std::string out;
    std::string prev_section;
    for (const auto &item : snapshot)
    {
        const std::string &name = item.first;
        if (filter && !filter(name))
        {
            continue;
        }
        const std::shared_ptr<metric> &ptr = item.second;
        if (nullptr == ptr)
        {
            continue;
        }

        std::string section;
        std::string key;
        if (!split_section_key(name, section, key))
        {
            continue;
        }

        if (!out.empty() && section != prev_section && !prev_section.empty())
        {
            out += '\n';
        }
        prev_section = section;

        out += section;
        out += '.';
        out += key;
        out += " = ";
        out += strip_trailing_angle_comment(format_metric_value(*ptr, key));
        out += '\n';
    }

    return out;
}

}  // namespace vstreamer
