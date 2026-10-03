#ifndef VSTREAMER_CORE_COMPONENT_HPP
#define VSTREAMER_CORE_COMPONENT_HPP

#include <string>
#include <string_view>

namespace vstreamer
{

class component
{
public:
    virtual ~component() = default;

    /* 0 on success, -ENOTSUP unknown key, -EINVAL bad value, other -errno. */
    virtual int configure(std::string_view key, std::string_view value) = 0;
    /* Fills *value (overwritten). 0 / -ENOTSUP / -EINVAL. Thread-safe. */
    virtual int query(std::string_view key, std::string *value) const = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_HPP
