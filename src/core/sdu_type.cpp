#include "core/sdu_type.hpp"

#include <cstring>

namespace vstreamer
{

namespace
{

struct sdu_type_name_entry
{
    sdu_type_e    type;
    const char   *name;
};

constexpr sdu_type_name_entry k_sdu_type_names[] = {
    {sdu_type_e::UNKNOWN, "UNKNOWN"},
    {sdu_type_e::NV12, "NV12"},
    {sdu_type_e::MJPEG, "MJPEG"},
    {sdu_type_e::H264_AU, "H264_AU"},
    {sdu_type_e::RTP, "RTP"},
    {sdu_type_e::STREAM_DGRAM, "STREAM_DGRAM"},
    {sdu_type_e::PCM, "PCM"},
    {sdu_type_e::CAPS_VIDEO_RAW, "CAPS_VIDEO_RAW"},
    {sdu_type_e::CAPS_VIDEO_CODED, "CAPS_VIDEO_CODED"},
    {sdu_type_e::CAPS_AUDIO, "CAPS_AUDIO"},
};

}  // namespace

const char *sdu_type_name(sdu_type_e t)
{
    for (const sdu_type_name_entry &e : k_sdu_type_names)
    {
        if (e.type == t)
        {
            return e.name;
        }
    }
    return "UNKNOWN";
}

bool parse_sdu_type(std::string_view name, sdu_type_e *out)
{
    if (nullptr == out)
    {
        return false;
    }
    for (const sdu_type_name_entry &e : k_sdu_type_names)
    {
        if (name == e.name)
        {
            *out = e.type;
            return true;
        }
    }
    return false;
}

}  // namespace vstreamer
