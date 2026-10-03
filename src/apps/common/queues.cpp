#include "apps/common/queues.hpp"

#include <algorithm>
#include <cstdlib>

namespace vstreamer::apps
{

[[nodiscard]] size_t queue_depth_from_env(const char *name, size_t default_val, size_t max_val)
{
    const char *v = std::getenv(name);
    if (nullptr == v || v[0] == '\0')
    {
        return default_val;
    }
    char              *end = nullptr;
    const unsigned long n = std::strtoul(v, &end, 10);
    if (end == v || n == 0)
    {
        return default_val;
    }
    return static_cast<size_t>(std::min(n, static_cast<unsigned long>(max_val)));
}

}  // namespace vstreamer::apps
