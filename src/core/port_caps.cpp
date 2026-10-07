#include "core/port_caps.hpp"

#include <cerrno>

#include <charconv>
#include <sstream>

namespace vstreamer
{

namespace
{

[[nodiscard]] bool parse_i32(std::string_view s, int32_t *out)
{
    if (nullptr == out || s.empty())
    {
        return false;
    }
    int32_t v = 0;
    const char *begin = s.data();
    const char *end = begin + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec != std::errc() || ptr != end)
    {
        return false;
    }
    *out = v;
    return true;
}

[[nodiscard]] std::string port_prefix(bool is_input)
{
    return is_input ? "inport" : "outport";
}

[[nodiscard]] const int32_constraint *find_field(const port_caps_entry &entry,
                                                 std::string_view name)
{
    for (const port_caps_field &f : entry.fields)
    {
        if (f.name == name)
        {
            return &f.constraint;
        }
    }
    return nullptr;
}

[[nodiscard]] bool field_matches(const port_caps_entry &entry, std::string_view name, int32_t v)
{
    const int32_constraint *c = find_field(entry, name);
    if (nullptr == c)
    {
        return true;
    }
    return c->matches(v);
}

}  // namespace

bool int32_constraint::matches(int32_t v) const
{
    if (any)
    {
        return true;
    }
    if (!list_values.empty())
    {
        for (int32_t x : list_values)
        {
            if (x == v)
            {
                return true;
            }
        }
        return false;
    }
    if (is_range)
    {
        return v >= range_min && v <= range_max;
    }
    return v == single;
}

std::string inport_size_key()
{
    return "inport.size";
}

std::string outport_size_key()
{
    return "outport.size";
}

std::string inport_caps_size_key(uint8_t port)
{
    return "inport-" + std::to_string(port) + ".caps.size";
}

std::string outport_caps_size_key(uint8_t port)
{
    return "outport-" + std::to_string(port) + ".caps.size";
}

std::string inport_caps_field_key(uint8_t port, size_t caps_index, std::string_view field)
{
    return "inport-" + std::to_string(port) + ".caps-" + std::to_string(caps_index) + "." +
           std::string(field);
}

std::string outport_caps_field_key(uint8_t port, size_t caps_index, std::string_view field)
{
    return "outport-" + std::to_string(port) + ".caps-" + std::to_string(caps_index) + "." +
           std::string(field);
}

bool parse_int32_constraint(std::string_view value, int32_constraint *out)
{
    if (nullptr == out)
    {
        return false;
    }
    int32_constraint c;
    if (value.empty())
    {
        return false;
    }
    const size_t dotdot = value.find("..");
    if (dotdot != std::string_view::npos)
    {
        int32_t lo = 0;
        int32_t hi = 0;
        if (!parse_i32(value.substr(0, dotdot), &lo) ||
            !parse_i32(value.substr(dotdot + 2), &hi) || lo > hi)
        {
            return false;
        }
        c.any = false;
        c.is_range = true;
        c.range_min = lo;
        c.range_max = hi;
        *out = c;
        return true;
    }
    if (value.find(',') != std::string_view::npos)
    {
        c.any = false;
        size_t start = 0;
        while (start <= value.size())
        {
            const size_t comma = value.find(',', start);
            const std::string_view part =
                value.substr(start, comma == std::string_view::npos ? value.size() - start
                                                                  : comma - start);
            int32_t v = 0;
            if (!parse_i32(part, &v))
            {
                return false;
            }
            c.list_values.push_back(v);
            if (comma == std::string_view::npos)
            {
                break;
            }
            start = comma + 1;
        }
        if (c.list_values.empty())
        {
            return false;
        }
        *out = c;
        return true;
    }
    int32_t single = 0;
    if (!parse_i32(value, &single))
    {
        return false;
    }
    c.any = false;
    c.single = single;
    *out = c;
    return true;
}

bool match(const port_caps_entry &entry, const video_raw_caps &caps)
{
    if (entry.sdu_type != sdu_type_e::CAPS_VIDEO_RAW)
    {
        return false;
    }
    return field_matches(entry, "width", caps.width) && field_matches(entry, "height", caps.height) &&
           field_matches(entry, "hor_stride", caps.hor_stride) &&
           field_matches(entry, "ver_stride", caps.ver_stride) &&
           field_matches(entry, "fps_num", caps.fps_num) &&
           field_matches(entry, "fps_den", caps.fps_den);
}

bool match(const port_caps_entry &entry, const video_coded_caps &caps)
{
    if (entry.sdu_type != sdu_type_e::CAPS_VIDEO_CODED)
    {
        return false;
    }
    return field_matches(entry, "width", caps.width) && field_matches(entry, "height", caps.height) &&
           field_matches(entry, "fps_num", caps.fps_num) &&
           field_matches(entry, "fps_den", caps.fps_den);
}

bool match(const port_caps_entry &entry, const audio_caps &caps)
{
    if (entry.sdu_type != sdu_type_e::CAPS_AUDIO)
    {
        return false;
    }
    return field_matches(entry, "sample_rate", caps.sample_rate) &&
           field_matches(entry, "channels", caps.channels);
}

bool match(const port_caps_entry &entry, const component_pdu &caps_pdu)
{
    if (!is_caps(caps_pdu.sdu_type) || entry.sdu_type != caps_pdu.sdu_type)
    {
        return false;
    }
    switch (caps_pdu.sdu_type)
    {
    case sdu_type_e::CAPS_VIDEO_RAW:
    {
        video_raw_caps raw {};
        if (read_caps(caps_pdu, &raw) != 0)
        {
            return false;
        }
        return match(entry, raw);
    }
    case sdu_type_e::CAPS_VIDEO_CODED:
    {
        video_coded_caps coded {};
        if (read_caps(caps_pdu, &coded) != 0)
        {
            return false;
        }
        return match(entry, coded);
    }
    case sdu_type_e::CAPS_AUDIO:
    {
        audio_caps aud {};
        if (read_caps(caps_pdu, &aud) != 0)
        {
            return false;
        }
        return match(entry, aud);
    }
    default:
        return false;
    }
}

int port_caps_query(const std::vector<port_desc> &ports, bool is_input, std::string_view key,
                    std::string *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    const std::string prefix = port_prefix(is_input);
    const std::string size_key = prefix + ".size";
    if (key == size_key)
    {
        *out = std::to_string(ports.size());
        return 0;
    }
    const std::string dash = prefix + "-";
    if (key.size() < dash.size() || key.substr(0, dash.size()) != dash)
    {
        return -ENOTSUP;
    }
    size_t i = dash.size();
    while (i < key.size() && key[i] >= '0' && key[i] <= '9')
    {
        ++i;
    }
    if (i == dash.size() || key[i] != '.')
    {
        return -EINVAL;
    }
    int32_t port_num = 0;
    if (!parse_i32(key.substr(dash.size(), i - dash.size()), &port_num) || port_num < 0)
    {
        return -EINVAL;
    }
    const size_t port_idx = static_cast<size_t>(port_num);
    if (port_idx >= ports.size())
    {
        return -EINVAL;
    }
    const std::string_view rest = key.substr(i + 1);
    if (rest == "caps.size")
    {
        *out = std::to_string(ports[port_idx].caps.size());
        return 0;
    }
    const std::string caps_prefix = "caps-";
    if (rest.size() < caps_prefix.size() + 1 || rest.substr(0, caps_prefix.size()) != caps_prefix)
    {
        return -ENOTSUP;
    }
    size_t j = caps_prefix.size();
    while (j < rest.size() && rest[j] >= '0' && rest[j] <= '9')
    {
        ++j;
    }
    if (j == caps_prefix.size() || rest[j] != '.')
    {
        return -EINVAL;
    }
    int32_t caps_idx = 0;
    if (!parse_i32(rest.substr(caps_prefix.size(), j - caps_prefix.size()), &caps_idx) ||
        caps_idx < 0)
    {
        return -EINVAL;
    }
    const size_t entry_idx = static_cast<size_t>(caps_idx);
    const port_desc &pd = ports[port_idx];
    if (entry_idx >= pd.caps.size())
    {
        return -EINVAL;
    }
    const port_caps_entry &entry = pd.caps[entry_idx];
    const std::string_view field = rest.substr(j + 1);
    if (field == "sdu_type")
    {
        *out = sdu_type_name(entry.sdu_type);
        return 0;
    }
    const int32_constraint *fc = find_field(entry, field);
    if (nullptr == fc)
    {
        *out = "";
        return 0;
    }
    if (fc->any)
    {
        *out = "";
        return 0;
    }
    if (fc->is_range)
    {
        *out = std::to_string(fc->range_min) + ".." + std::to_string(fc->range_max);
        return 0;
    }
    if (!fc->list_values.empty())
    {
        std::ostringstream os;
        for (size_t k = 0; k < fc->list_values.size(); ++k)
        {
            if (k > 0)
            {
                os << ',';
            }
            os << fc->list_values[k];
        }
        *out = os.str();
        return 0;
    }
    *out = std::to_string(fc->single);
    return 0;
}

}  // namespace vstreamer
