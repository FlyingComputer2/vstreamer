#ifndef VSTREAMER_CORE_STREAM_HEADER_HPP
#define VSTREAMER_CORE_STREAM_HEADER_HPP

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace vstreamer
{

inline constexpr uint8_t k_stream_wire_version = 2;

/* v2 packet header on the wire; v1 path still uses the 2-byte prefix below until migrated. */
inline constexpr size_t k_stream_v2_header_len = 4;

inline constexpr unsigned k_stream_flag_version_shift = 3;
inline constexpr unsigned k_stream_flag_version_mask = 0x1F;
inline constexpr unsigned k_stream_flag_is_fec_shift = 2;
inline constexpr unsigned k_stream_flag_is_stream_data_shift = 1;
inline constexpr unsigned k_stream_flag_spare_mask = 0x01;

struct stream_header
{
    uint16_t sequence_number;
    bool     is_fec;
    bool     is_stream_data;
    uint8_t  ext_len;
};

struct stream_header_s
{
    uint16_t stream_sequence; /* big-endian on wire */
};

inline constexpr size_t k_stream_header_len = sizeof(uint16_t);

inline void stream_header_store_be16(uint8_t *wire, uint16_t seq)
{
    wire[0] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    wire[1] = static_cast<uint8_t>(seq & 0xFF);
}

[[nodiscard]] inline uint16_t stream_header_sequence_be16(const uint8_t *wire)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(wire[0]) << 8) |
                                 static_cast<uint16_t>(wire[1]));
}

[[nodiscard]] inline bool stream_datagram_len_ok(size_t len)
{
    return len >= k_stream_header_len + 4U;
}

[[nodiscard]] inline const uint8_t *stream_fec_shard(const uint8_t *payload, size_t len,
                                                     size_t *fec_len_out)
{
    if (fec_len_out == nullptr || !stream_datagram_len_ok(len))
    {
        return nullptr;
    }
    *fec_len_out = len - k_stream_header_len;
    return payload + k_stream_header_len;
}

[[nodiscard]] inline size_t stream_max_fec_shard(size_t max_datagram)
{
    if (max_datagram <= k_stream_v2_header_len)
    {
        return 0;
    }
    return max_datagram - k_stream_v2_header_len;
}

[[nodiscard]] inline size_t stream_max_raw_sdu(size_t max_datagram)
{
    return stream_max_fec_shard(max_datagram);
}

inline void stream_header_write(uint8_t *out, const stream_header &hdr)
{
    out[0] = static_cast<uint8_t>((hdr.sequence_number >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(hdr.sequence_number & 0xFF);
    uint8_t flag = static_cast<uint8_t>(k_stream_wire_version << k_stream_flag_version_shift);
    if (hdr.is_fec)
    {
        flag = static_cast<uint8_t>(flag | (1U << k_stream_flag_is_fec_shift));
    }
    if (hdr.is_stream_data)
    {
        flag = static_cast<uint8_t>(flag | (1U << k_stream_flag_is_stream_data_shift));
    }
    out[2] = flag;
    out[3] = 0; /* ext_len; no TLV types defined yet */
}

inline void stream_header_stamp_sequence(uint8_t *wire, uint16_t sequence_number)
{
    wire[0] = static_cast<uint8_t>((sequence_number >> 8) & 0xFF);
    wire[1] = static_cast<uint8_t>(sequence_number & 0xFF);
}

[[nodiscard]] inline int stream_header_parse(const uint8_t *data, size_t len, stream_header *hdr,
                                             const uint8_t **payload, size_t *payload_len)
{
    if (hdr == nullptr || payload == nullptr || payload_len == nullptr)
    {
        return -EINVAL;
    }
    if (len < k_stream_v2_header_len)
    {
        return -EMSGSIZE;
    }

    hdr->sequence_number = stream_header_sequence_be16(data);
    const uint8_t flag = data[2];
    hdr->ext_len = data[3];

    if (0 != (flag & k_stream_flag_spare_mask))
    {
        return -EPROTO;
    }

    const unsigned version =
        (static_cast<unsigned>(flag) >> k_stream_flag_version_shift) & k_stream_flag_version_mask;
    if (version != k_stream_wire_version)
    {
        return -EPROTO;
    }

    hdr->is_fec = 0 != (flag & (1U << k_stream_flag_is_fec_shift));
    hdr->is_stream_data = 0 != (flag & (1U << k_stream_flag_is_stream_data_shift));

    if (hdr->is_fec && !hdr->is_stream_data)
    {
        return -EPROTO;
    }

    const size_t ext_total = static_cast<size_t>(hdr->ext_len);
    if (len < k_stream_v2_header_len + ext_total)
    {
        return -EMSGSIZE;
    }

    size_t off = k_stream_v2_header_len;
    const size_t ext_end = off + ext_total;
    while (off < ext_end)
    {
        if (off + 2 > ext_end)
        {
            return -EMSGSIZE;
        }
        const uint8_t tlv_len = data[off + 1];
        const size_t  tlv_total = 2U + static_cast<size_t>(tlv_len);
        if (off + tlv_total > ext_end)
        {
            return -EMSGSIZE;
        }
        off += tlv_total;
    }

    *payload = data + k_stream_v2_header_len + ext_total;
    *payload_len = len - k_stream_v2_header_len - ext_total;
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_STREAM_HEADER_HPP
