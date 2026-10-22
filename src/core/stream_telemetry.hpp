#ifndef VSTREAMER_CORE_STREAM_TELEMETRY_HPP
#define VSTREAMER_CORE_STREAM_TELEMETRY_HPP

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace vstreamer
{

/* Receiver-local link counters (UDP + post-FEC gaps). Loss % is derived in the app. */
struct stream_link_counters
{
    uint64_t udp_packet_received = 0;
    uint64_t fec_packet_received = 0;
    uint64_t udp_gap_count = 0;
    uint64_t fec_gap_count = 0;
    uint64_t udp_bytes_received = 0;
    uint64_t fec_bytes_received = 0;
};

struct stream_link_report
{
    uint32_t             session_id = 0;
    uint16_t             report_seq = 0;
    uint64_t             timestamp_us = 0;
    stream_link_counters counters {};
    bool has_byte_totals = true;
};

struct stream_peer_link
{
    bool                 have = false;
    stream_link_report   report {};
    int64_t              age_ms = -1;
    int64_t              observed_interval_ms = -1;
    uint64_t             reports_received = 0;
    uint64_t             reports_lost = 0;
    uint64_t             reports_rejected = 0;
};

// Legacy reports have no header or byte totals. V2 starts with "VL", version,
// reserved.
inline constexpr size_t k_stream_link_report_v1_payload_len = 44;
inline constexpr size_t k_stream_link_report_payload_len = 64;

namespace detail
{

inline void put_be16(uint8_t *out, uint16_t v)
{
    out[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(v & 0xFF);
}

inline void put_be32(uint8_t *out, uint32_t v)
{
    out[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(v & 0xFF);
}

inline void put_be64(uint8_t *out, uint64_t v)
{
    put_be32(out, static_cast<uint32_t>(v >> 32));
    put_be32(out + 4, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
}

inline uint16_t get_be16(const uint8_t *in)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) |
                                 static_cast<uint16_t>(in[1]));
}

inline uint32_t get_be32(const uint8_t *in)
{
    return (static_cast<uint32_t>(in[0]) << 24) |
           (static_cast<uint32_t>(in[1]) << 16) |
           (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

inline uint64_t get_be64(const uint8_t *in)
{
    return (static_cast<uint64_t>(get_be32(in)) << 32) |
           static_cast<uint64_t>(get_be32(in + 4));
}

}  // namespace detail

inline void stream_link_report_encode_payload(const stream_link_report &r, uint8_t *out,
                                              size_t out_len)
{
    if (nullptr == out || out_len < k_stream_link_report_payload_len)
    {
        return;
    }
    out[0] = 'V';
    out[1] = 'L';
    out[2] = 2;
    out[3] = 0;
    out += 4;
    detail::put_be32(out + 0, r.session_id);
    detail::put_be64(out + 4, r.timestamp_us);
    detail::put_be64(out + 12, r.counters.udp_packet_received);
    detail::put_be64(out + 20, r.counters.udp_gap_count);
    detail::put_be64(out + 28, r.counters.fec_packet_received);
    detail::put_be64(out + 36, r.counters.fec_gap_count);
    detail::put_be64(out + 44, r.counters.udp_bytes_received);
    detail::put_be64(out + 52, r.counters.fec_bytes_received);
}

inline int stream_link_report_decode(const uint8_t *data, size_t len, stream_link_report *out)
{
    if (nullptr == data || nullptr == out)
    {
        return -EINVAL;
    }
    const bool legacy = len == k_stream_link_report_v1_payload_len;
    if (!legacy)
    {
        if (len != k_stream_link_report_payload_len)
        {
            return -EMSGSIZE;
        }
        if (data[0] != 'V' || data[1] != 'L' || data[2] != 2 || data[3] != 0)
        {
            return -EPROTO;
        }
        data += 4;
    }
    stream_link_report r {};
    r.has_byte_totals = !legacy;
    r.session_id = detail::get_be32(data + 0);
    if (0 == r.session_id)
    {
        return -EPROTO;
    }
    r.timestamp_us = detail::get_be64(data + 4);
    r.counters.udp_packet_received = detail::get_be64(data + 12);
    r.counters.udp_gap_count = detail::get_be64(data + 20);
    r.counters.fec_packet_received = detail::get_be64(data + 28);
    r.counters.fec_gap_count = detail::get_be64(data + 36);
    if (!legacy)
    {
        r.counters.udp_bytes_received = detail::get_be64(data + 44);
        r.counters.fec_bytes_received = detail::get_be64(data + 52);
    }
    *out = r;
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_STREAM_TELEMETRY_HPP
