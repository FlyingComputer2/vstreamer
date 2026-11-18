#ifndef VSTREAMER_CORE_STREAM_MEDIA_HEADER_HPP
#define VSTREAMER_CORE_STREAM_MEDIA_HEADER_HPP

#include <cstddef>
#include <cstdint>

namespace vstreamer
{

/*
 * Header of every media datagram (stream_sender -> stream_receiver), bits MSB first:
 *
 *   u8  flag = 1 | spare(7) = 0
 *   u16 sequence_number (BE): one counter over the stream's datagrams
 *   payload (the app SDU, e.g. one RTP packet)
 *
 * The receiver counts gaps in the sequence and marks the datagram after a gap as a
 * discontinuity. Loss protection (FEC) is the link's job: winject-l3 runs it per upstream. The
 * layout is the data shard of the winject-fec wire, so it is also what a vstreamer with its own
 * FEC set to none used to send. The reverse path (receiver link reports) has its own header,
 * stream_header.
 */
inline constexpr size_t  k_media_header_len = 3;
inline constexpr uint8_t k_media_flag = 0x80U;

inline void media_header_write(uint8_t *out, uint16_t sequence_number)
{
    out[0] = k_media_flag;
    out[1] = static_cast<uint8_t>(sequence_number >> 8);
    out[2] = static_cast<uint8_t>(sequence_number & 0xFFU);
}

/* False for anything but a media datagram with a payload (a short datagram, or one whose flag
 * is not exactly 0x80, such as an FEC parity shard from an older sender). */
[[nodiscard]] inline bool media_header_parse(const uint8_t *data, size_t len,
                                             uint16_t *sequence_number)
{
    if (nullptr == data || len <= k_media_header_len || k_media_flag != data[0])
    {
        return false;
    }
    if (nullptr != sequence_number)
    {
        *sequence_number =
            static_cast<uint16_t>((static_cast<uint16_t>(data[1]) << 8) | data[2]);
    }
    return true;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_STREAM_MEDIA_HEADER_HPP
