#include "core/sdu_caps.hpp"

#include <cerrno>
#include <cstring>

namespace vstreamer
{

namespace
{

template <typename T>
[[nodiscard]] component_pdu make_caps_pdu_impl(sdu_type_e caps_type, const T &caps, uint64_t ts_us,
                                               uint8_t port)
{
    component_pdu pdu;
    pdu.ts_us = ts_us;
    pdu.sdu_type = caps_type;
    pdu.port = port;
    pdu.sdu = shared_sized_buffer::copy_from(&caps, sizeof(T));
    return pdu;
}

}  // namespace

sdu_type_e caps_type_for(sdu_type_e data_type)
{
    switch (data_type)
    {
    case sdu_type_e::NV12:
        return sdu_type_e::CAPS_VIDEO_RAW;
    case sdu_type_e::MJPEG:
    case sdu_type_e::H264_AU:
        return sdu_type_e::CAPS_VIDEO_CODED;
    case sdu_type_e::PCM:
        return sdu_type_e::CAPS_AUDIO;
    default:
        return sdu_type_e::UNKNOWN;
    }
}

component_pdu make_caps_pdu(sdu_type_e caps_type, const video_raw_caps &caps, uint64_t ts_us,
                            uint8_t port)
{
    return make_caps_pdu_impl(caps_type, caps, ts_us, port);
}

component_pdu make_caps_pdu(sdu_type_e caps_type, const video_coded_caps &caps, uint64_t ts_us,
                            uint8_t port)
{
    return make_caps_pdu_impl(caps_type, caps, ts_us, port);
}

component_pdu make_caps_pdu(sdu_type_e caps_type, const audio_caps &caps, uint64_t ts_us,
                            uint8_t port)
{
    return make_caps_pdu_impl(caps_type, caps, ts_us, port);
}

}  // namespace vstreamer
