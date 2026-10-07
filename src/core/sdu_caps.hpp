#ifndef VSTREAMER_CORE_SDU_CAPS_HPP
#define VSTREAMER_CORE_SDU_CAPS_HPP

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <type_traits>

#include "core/component_pdu.hpp"

namespace vstreamer
{

struct video_raw_caps
{
    int32_t width = 0;
    int32_t height = 0;
    int32_t hor_stride = 0;
    int32_t ver_stride = 0;
    int32_t fps_num = 0;
    int32_t fps_den = 0;
};

struct video_coded_caps
{
    int32_t width = 0;
    int32_t height = 0;
    int32_t fps_num = 0;
    int32_t fps_den = 0;
};

struct audio_caps
{
    int32_t sample_rate = 0;
    int32_t channels = 0;
};

static_assert(std::is_trivially_copyable_v<video_raw_caps>);
static_assert(std::is_trivially_copyable_v<video_coded_caps>);
static_assert(std::is_trivially_copyable_v<audio_caps>);

[[nodiscard]] sdu_type_e caps_type_for(sdu_type_e data_type);

[[nodiscard]] component_pdu make_caps_pdu(sdu_type_e caps_type, const video_raw_caps &caps,
                                        uint64_t ts_us, uint8_t port);
[[nodiscard]] component_pdu make_caps_pdu(sdu_type_e caps_type, const video_coded_caps &caps,
                                        uint64_t ts_us, uint8_t port);
[[nodiscard]] component_pdu make_caps_pdu(sdu_type_e caps_type, const audio_caps &caps,
                                        uint64_t ts_us, uint8_t port);

template <typename T>
[[nodiscard]] int read_caps(const component_pdu &pdu, T *out);

template <>
[[nodiscard]] inline int read_caps<video_raw_caps>(const component_pdu &pdu, video_raw_caps *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    if (pdu.sdu_type != sdu_type_e::CAPS_VIDEO_RAW || pdu.sdu.size() != sizeof(video_raw_caps))
    {
        return -EINVAL;
    }
    std::memcpy(out, pdu.sdu.data(), sizeof(video_raw_caps));
    return 0;
}

template <>
[[nodiscard]] inline int read_caps<video_coded_caps>(const component_pdu &pdu,
                                                     video_coded_caps *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    if (pdu.sdu_type != sdu_type_e::CAPS_VIDEO_CODED ||
        pdu.sdu.size() != sizeof(video_coded_caps))
    {
        return -EINVAL;
    }
    std::memcpy(out, pdu.sdu.data(), sizeof(video_coded_caps));
    return 0;
}

template <>
[[nodiscard]] inline int read_caps<audio_caps>(const component_pdu &pdu, audio_caps *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    if (pdu.sdu_type != sdu_type_e::CAPS_AUDIO || pdu.sdu.size() != sizeof(audio_caps))
    {
        return -EINVAL;
    }
    std::memcpy(out, pdu.sdu.data(), sizeof(audio_caps));
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_SDU_CAPS_HPP
