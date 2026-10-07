#ifndef VSTREAMER_TESTS_TEST_PDU_HELPERS_HPP
#define VSTREAMER_TESTS_TEST_PDU_HELPERS_HPP

#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/shared_sized_buffer.hpp"

#include <cstdint>
#include <vector>

namespace vstreamer::test_pdu
{

inline uint32_t stream_dgram_counter_be32(const uint8_t *data, size_t len)
{
    if (len < 4U)
    {
        return 0U;
    }
    return (static_cast<uint32_t>(data[0]) << 24U) | (static_cast<uint32_t>(data[1]) << 16U) |
           (static_cast<uint32_t>(data[2]) << 8U) | static_cast<uint32_t>(data[3]);
}

inline component_pdu make_stream_dgram(uint32_t counter, size_t payload_bytes = 64)
{
    std::vector<uint8_t> storage(payload_bytes);
    storage[0] = static_cast<uint8_t>((counter >> 24) & 0xFF);
    storage[1] = static_cast<uint8_t>((counter >> 16) & 0xFF);
    storage[2] = static_cast<uint8_t>((counter >> 8) & 0xFF);
    storage[3] = static_cast<uint8_t>(counter & 0xFF);
    for (size_t i = 4; i < payload_bytes; i++)
    {
        storage[i] = static_cast<uint8_t>(i & 0xFF);
    }

    component_pdu pdu;
    pdu.ts_us = counter;
    pdu.seq = counter;
    pdu.sdu_type = sdu_type_e::STREAM_DGRAM;
    pdu.port = 0;
    pdu.flags = 0;
    pdu.sdu = shared_sized_buffer::copy_from(storage.data(), storage.size());
    return pdu;
}

inline component_pdu make_nv12(int w, int h, uint64_t ts_us)
{
    const size_t sz = static_cast<size_t>(w) * static_cast<size_t>(h) * 3U / 2U;
    std::vector<uint8_t> buf(sz, 0x10);
    component_pdu pdu;
    pdu.ts_us = ts_us;
    pdu.seq = ts_us;
    pdu.sdu_type = sdu_type_e::NV12;
    pdu.port = 0;
    pdu.flags = static_cast<uint8_t>(pdu_flag_e::KEY);
    pdu.sdu = shared_sized_buffer::copy_from(buf.data(), buf.size());
    return pdu;
}

inline component_pdu make_nv12_caps(int w, int h, int fps, uint64_t ts_us = 1)
{
    video_raw_caps caps {};
    caps.width = w;
    caps.height = h;
    caps.hor_stride = w;
    caps.ver_stride = h;
    caps.fps_num = fps;
    caps.fps_den = 1;
    return make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, ts_us, 0);
}

inline component_pdu make_h264_coded_caps(int w, int h, int fps = 30, uint64_t ts_us = 1)
{
    video_coded_caps caps {};
    caps.width = w;
    caps.height = h;
    caps.fps_num = fps;
    caps.fps_den = 1;
    return make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, caps, ts_us, 0);
}

inline component_pdu make_h264_au(const uint8_t *data, size_t len, uint64_t ts_us, bool key = true)
{
    component_pdu pdu;
    pdu.ts_us = ts_us;
    pdu.seq = ts_us;
    pdu.sdu_type = sdu_type_e::H264_AU;
    pdu.port = 0;
    pdu.flags = key ? static_cast<uint8_t>(pdu_flag_e::KEY) : 0;
    pdu.sdu = shared_sized_buffer::copy_from(data, len);
    return pdu;
}

}  // namespace vstreamer::test_pdu

#endif  // VSTREAMER_TESTS_TEST_PDU_HELPERS_HPP
