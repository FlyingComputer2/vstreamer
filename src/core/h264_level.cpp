#include "core/h264_level.hpp"

#include <algorithm>

namespace vstreamer
{
namespace
{

struct level_row
{
    int level_idc;
    int max_mbps;
    int max_fs;
    int max_br_kbps;
};

constexpr level_row k_levels[] = {
    {10, 1485, 99, 64},
    {11, 3000, 396, 192},
    {12, 6000, 396, 384},
    {13, 11880, 396, 768},
    {20, 11880, 396, 2000},
    {21, 19800, 792, 4000},
    {22, 20250, 1620, 4000},
    {30, 40500, 1620, 10000},
    {31, 108000, 3600, 14000},
    {32, 216000, 5120, 20000},
    {40, 245760, 8192, 20000},
    {41, 245760, 8192, 50000},
    {42, 522240, 8704, 50000},
    {50, 589824, 22080, 135000},
    {51, 983040, 36864, 240000},
    {52, 2073600, 36864, 240000},
};

int ceil_div16(int v)
{
    return (v + 15) / 16;
}

}  // namespace

int h264_level_for_size(int w, int h, int fps, int kbps)
{
    if (w < 2 || h < 2 || fps <= 0)
    {
        return 31;
    }
    const int mb_w = ceil_div16(w);
    const int mb_h = ceil_div16(h);
    const int max_fs = mb_w * mb_h;
    const int mbps = max_fs * fps;

    for (const level_row &row : k_levels)
    {
        if (row.max_fs < max_fs)
        {
            continue;
        }
        if (row.max_mbps < mbps)
        {
            continue;
        }
        if (kbps > 0 &&
            static_cast<double>(kbps) > static_cast<double>(row.max_br_kbps) * 1.25)
        {
            continue;
        }
        return row.level_idc;
    }
    return k_levels[sizeof(k_levels) / sizeof(k_levels[0]) - 1].level_idc;
}

}  // namespace vstreamer
