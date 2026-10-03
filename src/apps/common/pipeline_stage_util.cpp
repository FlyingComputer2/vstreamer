#include "apps/common/pipeline_stage_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace vstreamer::apps
{

int cfg_str(vstreamer::component &c, const char *key, const char *val)
{
    std::string_view k(key);
    std::string_view v(val);
    return c.configure(k, v);
}

int open_stage(const char *name, int rc)
{
    if (rc < 0)
    {
        std::fprintf(stderr, "open failed: %s (%d", name, rc);
        if (-rc > 0 && -rc < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-rc));
        }
        std::fprintf(stderr, ")\n");
    }
    return rc;
}

}  // namespace vstreamer::apps
