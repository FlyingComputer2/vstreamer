#ifndef VSTREAMER_APPS_PIPELINE_STAGE_UTIL_HPP
#define VSTREAMER_APPS_PIPELINE_STAGE_UTIL_HPP

#include "core/component.hpp"

namespace vstreamer::apps
{

int cfg_str(vstreamer::component &c, const char *key, const char *val);
int open_stage(const char *name, int rc);

}  // namespace vstreamer::apps

#endif
