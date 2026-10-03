#ifndef VSTREAMER_APPS_CPU_MAP_HPP
#define VSTREAMER_APPS_CPU_MAP_HPP

#include <string>
#include <string_view>
#include <vector>

namespace vstreamer::apps
{

struct cpu_stage_map
{
    int              source = 0;
    int              jpeg = 1;
    int              encode = 2;
    int              rx = 3;
    std::vector<int> jpeg_workers {4, 5, 6, 7};
};

/* Parse VSTREAMER_CPU_MAP or use defaults. Malformed input → warning on stderr + defaults. */
cpu_stage_map parse_cpu_map(std::string_view spec);

std::string format_cpulist(const std::vector<int> &cpus);

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_CPU_MAP_HPP
