#ifndef VSTREAMER_CORE_KEY_UTIL_HPP
#define VSTREAMER_CORE_KEY_UTIL_HPP

#include <charconv>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace vstreamer
{

/* Decimal integer, whole string, no leading '+', optional '-'. */
inline int key_parse_i64(std::string_view s, int64_t *out)
{
    if (nullptr == out || s.empty())
    {
        return -EINVAL;
    }
    if ('+' == s.front())
    {
        return -EINVAL;
    }

    int64_t v = 0;
    const auto *begin = s.data();
    const auto *end = s.data() + s.size();
    const std::from_chars_result r = std::from_chars(begin, end, v, 10);
    if (r.ec != std::errc() || r.ptr != end)
    {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

/* C-string overload: base 10 only (no octal/hex surprises). */
inline int key_parse_i64(const char *s, int64_t *out)
{
    if (nullptr == s || nullptr == out)
    {
        return -EINVAL;
    }
    return key_parse_i64(std::string_view(s), out);
}

/* Auto base (0): decimal, or 0x hex when used for V4L2 control ids. */
inline int key_parse_i64_auto(std::string_view s, int64_t *out)
{
    if (nullptr == out || s.empty())
    {
        return -EINVAL;
    }

    char *end = nullptr;
    errno = 0;
    char buf[128];
    if (s.size() >= sizeof(buf))
    {
        return -EINVAL;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    const int64_t v = std::strtoll(buf, &end, 0);
    if (0 != errno || end == buf || *end != '\0')
    {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

inline int key_parse_double(std::string_view s, double *out)
{
    if (nullptr == out || s.empty())
    {
        return -EINVAL;
    }
    char buf[128];
    if (s.size() >= sizeof(buf))
    {
        return -EINVAL;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(buf, &end);
    if (0 != errno || end == buf || *end != '\0')
    {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

inline int key_parse_double(const char *s, double *out)
{
    if (nullptr == s || nullptr == out)
    {
        return -EINVAL;
    }
    return key_parse_double(std::string_view(s), out);
}

inline int key_format_i64(int64_t v, char *buf, size_t n)
{
    if (nullptr == buf || 0 == n)
    {
        return -EINVAL;
    }
    if (std::snprintf(buf, n, "%" PRId64, v) < 0)
    {
        return -EINVAL;
    }
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_KEY_UTIL_HPP
