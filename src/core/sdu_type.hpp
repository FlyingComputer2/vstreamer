#ifndef VSTREAMER_CORE_SDU_TYPE_HPP
#define VSTREAMER_CORE_SDU_TYPE_HPP

#include <cstdint>

#include <string_view>

namespace vstreamer
{

enum class sdu_type_e : uint16_t
{
    UNKNOWN = 0,
    NV12,
    MJPEG,
    H264_AU,
    RTP,
    STREAM_DGRAM,
    PCM,

    CAPS_BASE = 0x8000,
    CAPS_VIDEO_RAW = CAPS_BASE,
    CAPS_VIDEO_CODED,
    CAPS_AUDIO,
};

[[nodiscard]] constexpr bool is_caps(sdu_type_e t)
{
    return (static_cast<uint16_t>(t) & 0x8000U) != 0;
}

[[nodiscard]] const char *sdu_type_name(sdu_type_e t);
[[nodiscard]] bool          parse_sdu_type(std::string_view name, sdu_type_e *out);

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_SDU_TYPE_HPP
