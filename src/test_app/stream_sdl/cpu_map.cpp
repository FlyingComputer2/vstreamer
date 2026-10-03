#include "test_app/stream_sdl/cpu_map.hpp"

#include "core/key_util.hpp"

#include <cstdio>
#include <cstring>

namespace vstreamer::test_app
{
namespace
{

cpu_stage_map default_map()
{
    return cpu_stage_map {};
}

bool parse_cpulist(std::string_view spec, std::vector<int> *out)
{
    if (nullptr == out)
    {
        return false;
    }
    out->clear();
    if (spec.empty())
    {
        return false;
    }
    size_t i = 0;
    while (i < spec.size())
    {
        while (i < spec.size() && (spec[i] == ',' || spec[i] == ' '))
        {
            i++;
        }
        if (i >= spec.size())
        {
            break;
        }
        size_t j = i;
        while (j < spec.size() && spec[j] != ',')
        {
            j++;
        }
        const std::string_view token = spec.substr(i, j - i);
        size_t                   dash = token.find('-');
        if (dash != std::string_view::npos && dash > 0 && dash + 1 < token.size())
        {
            int64_t a = 0;
            int64_t b = 0;
            std::string left(token.substr(0, dash));
            std::string right(token.substr(dash + 1));
            if (key_parse_i64(left.c_str(), &a) < 0 || key_parse_i64(right.c_str(), &b) < 0 || a > b)
            {
                return false;
            }
            for (int64_t c = a; c <= b; c++)
            {
                out->push_back(static_cast<int>(c));
            }
        }
        else
        {
            int64_t v = 0;
            std::string tmp(token);
            if (key_parse_i64(tmp.c_str(), &v) < 0)
            {
                return false;
            }
            out->push_back(static_cast<int>(v));
        }
        i = j + 1;
    }
    return !out->empty();
}

bool set_stage_int(std::string_view /*stage*/, std::string_view val, int *target)
{
    int64_t v = 0;
    std::string tmp(val);
    if (key_parse_i64(tmp.c_str(), &v) < 0)
    {
        return false;
    }
    *target = static_cast<int>(v);
    return true;
}

}  // namespace

cpu_stage_map parse_cpu_map(std::string_view spec)
{
    cpu_stage_map m = default_map();
    if (spec.empty())
    {
        return m;
    }
    bool ok = true;
    size_t start = 0;
    while (start <= spec.size())
    {
        size_t end = spec.find(';', start);
        if (end == std::string_view::npos)
        {
            end = spec.size();
        }
        std::string_view entry = spec.substr(start, end - start);
        const size_t     eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0)
        {
            ok = false;
            break;
        }
        const std::string_view stage = entry.substr(0, eq);
        const std::string_view val = entry.substr(eq + 1);
        if (stage == "source")
        {
            ok = set_stage_int(stage, val, &m.source);
        }
        else if (stage == "jpeg")
        {
            ok = set_stage_int(stage, val, &m.jpeg);
        }
        else if (stage == "encode")
        {
            ok = set_stage_int(stage, val, &m.encode);
        }
        else if (stage == "rx")
        {
            ok = set_stage_int(stage, val, &m.rx);
        }
        else if (stage == "jpeg_workers")
        {
            ok = parse_cpulist(val, &m.jpeg_workers);
        }
        else
        {
            ok = false;
            break;
        }
        if (end == spec.size())
        {
            break;
        }
        start = end + 1;
    }
    if (!ok)
    {
        std::fprintf(stderr, "cpu_map: malformed VSTREAMER_CPU_MAP; using defaults\n");
        return default_map();
    }
    return m;
}

std::string format_cpulist(const std::vector<int> &cpus)
{
    std::string out;
    for (size_t i = 0; i < cpus.size(); i++)
    {
        if (i > 0)
        {
            out.push_back(',');
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", cpus[i]);
        out += buf;
    }
    return out;
}

}  // namespace vstreamer::test_app
