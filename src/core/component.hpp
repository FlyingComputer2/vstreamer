#ifndef VSTREAMER_CORE_COMPONENT_HPP
#define VSTREAMER_CORE_COMPONENT_HPP

#include <cstdint>

#include <memory>
#include <string>
#include <string_view>

#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>


namespace vstreamer
{

/* Typed query key: 64-bit FNV-1a of the string key name, so one name serves both forms.
 * Use it as a constant (`constexpr uint64_t k = query_key("dropped");`) to keep hashing
 * off hot paths. Keys of one component that collide fail to compile as duplicate `case`
 * labels when the component switches on them. */
[[nodiscard]] constexpr uint64_t query_key(std::string_view name) noexcept
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const char ch : name)
    {
        h ^= static_cast<uint8_t>(ch);
        h *= 0x100000001b3ULL;
    }
    return h;
}

class component
{
public:
    virtual ~component() = default;

    /* 0 on success, -ENOTSUP unknown key, -EINVAL bad value, other -errno. */
    virtual int configure(std::string_view key, std::string_view value) = 0;
    /* Fills *value (overwritten). 0 / -ENOTSUP / -EINVAL. Thread-safe. */
    virtual int query(std::string_view key, std::string *value) const = 0;

    /* Typed queries by query_key(): no string compare, formatting or allocation. Each
     * typed key has exactly one value type; the other overloads return -ENOTSUP for it.
     * Same semantics as the string form of the key (read-to-reset keys reset here too).
     * Components that override these must add `using component::query;` (or the base
     * they derive from) so the string overload stays visible. */
    virtual int query(uint64_t key, uint64_t *value) const
    {
        (void)key;
        (void)value;
        return -ENOTSUP;
    }
    virtual int query(uint64_t key, int64_t *value) const
    {
        (void)key;
        (void)value;
        return -ENOTSUP;
    }
    virtual int query(uint64_t key, double *value) const
    {
        (void)key;
        (void)value;
        return -ENOTSUP;
    }

protected:
    /* For string query(): answers `key` from the typed u64 / i64 overloads, formatted as
     * decimal. Returns -ENOTSUP when neither knows the key, so the caller falls through
     * to its string-only keys. Doubles need a per-key format, so the caller formats them. */
    int query_typed_as_string(std::string_view key, std::string *value) const
    {
        const uint64_t k = query_key(key);
        char           buf[24];
        uint64_t       u = 0;
        int            r = query(k, &u);
        if (0 == r)
        {
            std::snprintf(buf, sizeof(buf), "%" PRIu64, u);
            *value = buf;
            return 0;
        }
        if (-ENOTSUP != r)
        {
            return r;
        }
        int64_t i = 0;
        r = query(k, &i);
        if (0 == r)
        {
            std::snprintf(buf, sizeof(buf), "%" PRId64, i);
            *value = buf;
        }
        return r;
    }

};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_HPP
