#include "core/rtp_h264.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace vstreamer
{
namespace
{

constexpr int k_rtp_hdr = 12;
constexpr int k_max_rtp = 1500;
constexpr int k_max_param = 256;
constexpr int k_capture_ext_words = 3;
constexpr int k_capture_ext_total = 4 + k_capture_ext_words * 4;
/* RFC 5285 one-byte header extension id 2: capture instant, CLOCK_REALTIME ns (BE u64). */
constexpr uint8_t k_capture_ext_id = 2;
constexpr uint8_t k_capture_ext_len_bytes = 8;

uint32_t pts_to_rtp_ts(int64_t pts, int fps)
{
    const int f = fps > 0 ? fps : 30;
    return static_cast<uint32_t>((pts * 90000) / f);
}

int nal_type(const uint8_t *nal, size_t len)
{
    if (nullptr == nal || 0 == len)
    {
        return -1;
    }
    return nal[0] & 0x1f;
}

void parse_annexb_nals(const uint8_t *data, size_t size,
                       std::vector<std::pair<const uint8_t *, int>> *nals)
{
    nals->clear();
    size_t i = 0;
    while (i + 3 < size)
    {
        int sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            sc = 3;
        }
        else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
                 data[i + 3] == 1)
        {
            sc = 4;
        }
        else
        {
            ++i;
            continue;
        }
        const size_t start = i + sc;
        size_t         j = start;
        while (j + 3 < size)
        {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (data[j + 2] == 0 && j + 3 < size && data[j + 3] == 1)))
            {
                break;
            }
            ++j;
        }
        if (j + 3 >= size)
        {
            j = size;
        }
        const int len = static_cast<int>(j - start);
        if (len > 0)
        {
            nals->emplace_back(data + start, len);
        }
        i = j;
    }
}

}  // namespace

rtp_h264_packer::rtp_h264_packer(rtp_h264_config cfg_in) : cfg(cfg_in)
{
    seq = static_cast<uint16_t>(getpid() ^ static_cast<unsigned>(time(nullptr)));
}

void rtp_h264_packer::reset()
{
    queue.clear();
    sps_len = 0;
    pps_len = 0;
}

int rtp_h264_packer::append_datagram(const uint8_t *payload, int plen, int marker, uint32_t ts,
                                     int64_t capture_rt_ns)
{
    const bool have_capture = capture_rt_ns > 0;
    const int  hdr_extra = have_capture ? k_capture_ext_total : 0;
    const int  total = k_rtp_hdr + hdr_extra + plen;
    if (plen < 0 || total > cfg.mtu || total > k_max_rtp)
    {
        return -EINVAL;
    }

    std::vector<uint8_t> pkt(static_cast<size_t>(total));
    pkt[0] = static_cast<uint8_t>(have_capture ? 0x90 : 0x80);
    pkt[1] = static_cast<uint8_t>(cfg.payload_type | (marker ? 0x80 : 0));
    pkt[2] = static_cast<uint8_t>(seq >> 8);
    pkt[3] = static_cast<uint8_t>(seq & 0xff);
    pkt[4] = static_cast<uint8_t>(ts >> 24);
    pkt[5] = static_cast<uint8_t>(ts >> 16);
    pkt[6] = static_cast<uint8_t>(ts >> 8);
    pkt[7] = static_cast<uint8_t>(ts);
    pkt[8] = static_cast<uint8_t>(cfg.ssrc >> 24);
    pkt[9] = static_cast<uint8_t>(cfg.ssrc >> 16);
    pkt[10] = static_cast<uint8_t>(cfg.ssrc >> 8);
    pkt[11] = static_cast<uint8_t>(cfg.ssrc);
    if (have_capture)
    {
        pkt[12] = 0xbe;
        pkt[13] = 0xde;
        pkt[14] = 0;
        pkt[15] = static_cast<uint8_t>(k_capture_ext_words);
        pkt[16] = static_cast<uint8_t>((k_capture_ext_id << 4) | (k_capture_ext_len_bytes - 1));
        uint64_t rt_be = static_cast<uint64_t>(capture_rt_ns);
        for (int i = 0; i < 8; ++i)
        {
            pkt[17 + i] = static_cast<uint8_t>((rt_be >> (56 - 8 * i)) & 0xff);
        }
        pkt[25] = 0;
        pkt[26] = 0;
        pkt[27] = 0;
    }
    std::memcpy(pkt.data() + k_rtp_hdr + hdr_extra, payload, static_cast<size_t>(plen));
    seq++;
    queue.push_back(std::move(pkt));
    return 0;
}

int rtp_h264_packer::send_nal(const uint8_t *nal, int len, int marker, uint32_t ts,
                              int64_t capture_rt_ns)
{
    const bool have_capture = capture_rt_ns > 0;
    const int  hdr_extra = have_capture ? k_capture_ext_total : 0;
    const int  max_single = cfg.mtu - k_rtp_hdr - hdr_extra;
    if (len <= 0)
    {
        return 0;
    }
    if (len <= max_single)
    {
        return append_datagram(nal, len, marker, ts, capture_rt_ns);
    }

    const int max_fu = cfg.mtu - k_rtp_hdr - hdr_extra - 2;
    if (max_fu < 1)
    {
        return -EINVAL;
    }

    const uint8_t type = nal[0] & 0x1f;
    const uint8_t nri = nal[0] & 0x60;
    const uint8_t *p = nal + 1;
    int            left = len - 1;
    int            first = 1;
    while (left > 0)
    {
        const int chunk = left < max_fu ? left : max_fu;
        const int last = (left - chunk) == 0;
        uint8_t   fu[k_max_rtp];
        fu[0] = static_cast<uint8_t>(nri | 28);
        fu[1] = static_cast<uint8_t>(type | (first ? 0x80 : 0) | (last ? 0x40 : 0));
        std::memcpy(fu + 2, p, static_cast<size_t>(chunk));
        if (append_datagram(fu, chunk + 2, marker && last, ts, capture_rt_ns) < 0)
        {
            return -EINVAL;
        }
        p += chunk;
        left -= chunk;
        first = 0;
    }
    return 0;
}

void rtp_h264_packer::cache_param(const uint8_t *nal, int len)
{
    const int type = nal[0] & 0x1f;
    if (len <= 0 || len > k_max_param)
    {
        return;
    }
    if (7 == type)
    {
        std::memcpy(sps, nal, static_cast<size_t>(len));
        sps_len = len;
    }
    else if (8 == type)
    {
        std::memcpy(pps, nal, static_cast<size_t>(len));
        pps_len = len;
    }
}

int rtp_h264_packer::pack_annexb(const uint8_t *data, size_t size, int64_t pts,
                                 int64_t capture_rt_ns)
{
    const uint32_t ts = pts_to_rtp_ts(pts, cfg.fps);
    return pack_annexb_rtp_ts(data, size, ts, capture_rt_ns);
}

int rtp_h264_packer::pack_annexb_rtp_ts(const uint8_t *data, size_t size, uint32_t rtp_ts,
                                       int64_t capture_rt_ns)
{
    queue.clear();
    const uint32_t ts = rtp_ts;

    std::vector<std::pair<const uint8_t *, int>> nals;
    parse_annexb_nals(data, size, &nals);

    if (nals.empty() && size > 0)
    {
        int off = 0;
        if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] > 0 &&
            data[3] < static_cast<uint8_t>(size))
        {
            off = 4;
        }
        nals.emplace_back(data + off, static_cast<int>(size) - off);
    }

    bool au_has_idr = false;
    bool au_has_sps = false;
    bool au_has_pps = false;
    std::vector<std::pair<const uint8_t *, int>> emit;
    for (const auto &nal : nals)
    {
        const int type = nal_type(nal.first, static_cast<size_t>(nal.second));
        cache_param(nal.first, nal.second);
        if (9 == type)
        {
            continue;
        }
        if (5 == type)
        {
            au_has_idr = true;
        }
        if (7 == type)
        {
            au_has_sps = true;
        }
        if (8 == type)
        {
            au_has_pps = true;
        }
        emit.push_back(nal);
    }

    const int64_t capture_rt = capture_rt_ns > 0 ? capture_rt_ns : 0;

    bool injected_cached_sps = false;
    bool injected_cached_pps = false;
    for (size_t n = 0; n < emit.size(); ++n)
    {
        const int type = nal_type(emit[n].first, static_cast<size_t>(emit[n].second));
        if (au_has_idr && type >= 1 && type <= 5)
        {
            if (!au_has_sps && !injected_cached_sps && sps_len > 0)
            {
                if (send_nal(sps, sps_len, 0, ts, capture_rt) < 0)
                {
                    return -EINVAL;
                }
                injected_cached_sps = true;
            }
            if (!au_has_pps && !injected_cached_pps && pps_len > 0)
            {
                if (send_nal(pps, pps_len, 0, ts, capture_rt) < 0)
                {
                    return -EINVAL;
                }
                injected_cached_pps = true;
            }
        }
        const int marker = (n + 1 == emit.size()) ? 1 : 0;
        if (send_nal(emit[n].first, emit[n].second, marker, ts, capture_rt) < 0)
        {
            return -EINVAL;
        }
    }
    return 0;
}

int rtp_h264_packer::pop_datagram(uint8_t *dst, size_t cap)
{
    if (queue.empty())
    {
        return -EAGAIN;
    }
    const auto &front = queue.front();
    if (front.size() > cap)
    {
        return -ENOMEM;
    }
    std::memcpy(dst, front.data(), front.size());
    const int n = static_cast<int>(front.size());
    queue.pop_front();
    return n;
}

void rtp_h264_depacketizer::reset()
{
    fu_buf.clear();
    fu_active = false;
    building_au.clear();
    have_building_ts = false;
    building_key = false;
    building_damaged = false;
    last_seq = 0;
    have_seq = false;
    gap_packets = 0;
    received_packets = 0;
    loss = 0.f;
    last_au_frame_pts = 0;
    last_au_capture_rt_ns = 0;
    last_au_key = false;
    need_idr_count = 0;
    nal_dropped_count = 0;
    rtp_reordered_count = 0;
    building_capture_rt_ns = 0;
    completed_aus.clear();
}

bool rtp_h264_depacketizer::parse_rtp(const uint8_t *datagram, size_t len,
                                      parsed_rtp *out) const
{
    if (nullptr == out || nullptr == datagram || len < k_rtp_hdr)
    {
        return false;
    }
    if ((datagram[0] & 0xc0) != 0x80)
    {
        return false;
    }

    size_t pkt_len = len;
    if ((datagram[0] & 0x20) != 0)
    {
        if (0 == pkt_len)
        {
            return false;
        }
        const size_t pad = datagram[pkt_len - 1];
        if (pad >= pkt_len)
        {
            return false;
        }
        pkt_len -= pad;
    }

    const int cc = datagram[0] & 0x0f;
    size_t    off = k_rtp_hdr + static_cast<size_t>(cc) * 4;
    if (pkt_len < off)
    {
        return false;
    }

    int64_t capture_rt = 0;
    if ((datagram[0] & 0x10) != 0)
    {
        if (pkt_len < off + 4)
        {
            return false;
        }
        const uint16_t profile =
            static_cast<uint16_t>((static_cast<uint16_t>(datagram[off]) << 8) | datagram[off + 1]);
        if (profile == 0xbede)
        {
            const uint16_t ext_words = static_cast<uint16_t>(
                (static_cast<uint16_t>(datagram[off + 2]) << 8) | datagram[off + 3]);
            const size_t ext_total = 4 + static_cast<size_t>(ext_words) * 4;
            if (pkt_len < off + ext_total)
            {
                return false;
            }
            if (ext_words >= k_capture_ext_words && pkt_len >= off + 12)
            {
                const uint8_t id_len = datagram[off + 4];
                const uint8_t ext_id = id_len >> 4;
                const uint8_t ext_len = (id_len & 0x0f) + 1;
                if (ext_id == k_capture_ext_id && ext_len == k_capture_ext_len_bytes &&
                    pkt_len >= off + 4 + ext_len)
                {
                    uint64_t rt_be = 0;
                    for (int i = 0; i < 8; ++i)
                    {
                        rt_be = (rt_be << 8) | datagram[off + 5 + i];
                    }
                    capture_rt = static_cast<int64_t>(rt_be);
                }
            }
            off += ext_total;
        }
    }

    if (pkt_len <= off)
    {
        return false;
    }

    out->seq = static_cast<uint16_t>((datagram[2] << 8) | datagram[3]);
    out->ts = (static_cast<uint32_t>(datagram[4]) << 24) |
              (static_cast<uint32_t>(datagram[5]) << 16) |
              (static_cast<uint32_t>(datagram[6]) << 8) | static_cast<uint32_t>(datagram[7]);
    out->marker = (datagram[1] & 0x80) != 0;
    out->payload = datagram + off;
    out->plen = pkt_len - off;
    out->capture_rt_ns = capture_rt;
    return true;
}

void rtp_h264_depacketizer::note_sequence(uint16_t seq)
{
    if (!have_seq)
    {
        have_seq = true;
        last_seq = seq;
        ++received_packets;
        return;
    }

    const int16_t diff = static_cast<int16_t>(seq - last_seq);
    if (diff > 1)
    {
        if (fu_active)
        {
            abort_fu();
        }
        gap_packets += static_cast<uint64_t>(diff - 1);
    }

    last_seq = seq;
    ++received_packets;
    if (gap_packets > 0)
    {
        loss = static_cast<float>(gap_packets) /
               static_cast<float>(received_packets + gap_packets);
    }
}

void rtp_h264_depacketizer::begin_au_if_needed(uint32_t ts)
{
    if (!have_building_ts)
    {
        building_ts = ts;
        have_building_ts = true;
    }
}

void rtp_h264_depacketizer::append_annexb_nal(const uint8_t *nal, size_t len)
{
    if (nullptr == nal || 0 == len)
    {
        return;
    }
    building_au.push_back(0);
    building_au.push_back(0);
    building_au.push_back(0);
    building_au.push_back(1);
    building_au.insert(building_au.end(), nal, nal + len);
    if (5 == nal_type(nal, len))
    {
        building_key = true;
    }
}

void rtp_h264_depacketizer::abort_fu()
{
    fu_active = false;
    fu_buf.clear();
    nal_dropped_count++;
    building_damaged = true;
}

void rtp_h264_depacketizer::note_au_timestamps(uint32_t ts, int64_t capture_rt_ns)
{
    last_au_frame_pts = (static_cast<int64_t>(ts) * fps) / 90000;
    last_au_capture_rt_ns = capture_rt_ns;
}

void rtp_h264_depacketizer::finish_building_au(uint32_t ts, int64_t capture_rt_ns)
{
    if (building_au.empty())
    {
        have_building_ts = false;
        building_key = false;
        building_damaged = false;
        building_capture_rt_ns = 0;
        return;
    }

    completed_au_s item;
    item.bytes = building_au;
    item.ts = ts;
    item.capture_rt_ns = capture_rt_ns;
    item.key = building_key;
    item.damaged = building_damaged;
    completed_aus.push_back(std::move(item));

    building_au.clear();
    have_building_ts = false;
    building_key = false;
    building_damaged = false;
    building_capture_rt_ns = 0;
}

int rtp_h264_depacketizer::pop_completed_au(std::vector<uint8_t> *au_out)
{
    if (nullptr == au_out || completed_aus.empty())
    {
        return 0;
    }
    const completed_au_s item = std::move(completed_aus.front());
    completed_aus.pop_front();
    au_out->assign(item.bytes.begin(), item.bytes.end());
    last_au_key = item.key;
    if (item.damaged)
    {
        need_idr_count++;
    }
    note_au_timestamps(item.ts, item.capture_rt_ns);
    return 1;
}

int rtp_h264_depacketizer::process_payload(const parsed_rtp &rtp)
{
    if (rtp.plen == 0)
    {
        return 0;
    }

    const uint8_t nal_hdr = rtp.payload[0];
    const uint8_t type = nal_hdr & 0x1f;

    if (24 == type)
    {
        size_t off = 1;
        while (off + 2 <= rtp.plen)
        {
            const size_t sz =
                (static_cast<size_t>(rtp.payload[off]) << 8) | rtp.payload[off + 1];
            off += 2;
            if (off + sz > rtp.plen)
            {
                return -EINVAL;
            }
            append_annexb_nal(rtp.payload + off, sz);
            off += sz;
        }
        return 0;
    }

    if (type >= 1 && type <= 23)
    {
        append_annexb_nal(rtp.payload, rtp.plen);
        return 0;
    }

    if (28 != type)
    {
        return 0;
    }

    if (rtp.plen < 2)
    {
        return -EINVAL;
    }

    const uint8_t fu_hdr = rtp.payload[1];
    const bool    start = (fu_hdr & 0x80) != 0;
    const bool    end = (fu_hdr & 0x40) != 0;
    const uint8_t nal_t = fu_hdr & 0x1f;

    if (start)
    {
        fu_buf.clear();
        fu_buf.push_back(static_cast<uint8_t>((nal_hdr & 0xe0) | nal_t));
        fu_buf.insert(fu_buf.end(), rtp.payload + 2, rtp.payload + rtp.plen);
        fu_active = true;
    }
    else if (fu_active)
    {
        fu_buf.insert(fu_buf.end(), rtp.payload + 2, rtp.payload + rtp.plen);
    }
    else
    {
        abort_fu();
        return 0;
    }

    if (end && fu_active)
    {
        append_annexb_nal(fu_buf.data(), fu_buf.size());
        fu_active = false;
        fu_buf.clear();
    }
    return 0;
}

int rtp_h264_depacketizer::feed(const uint8_t *datagram, size_t len, std::vector<uint8_t> *au_out)
{
    if (nullptr == datagram || nullptr == au_out)
    {
        return -EINVAL;
    }

    parsed_rtp rtp {};
    if (!parse_rtp(datagram, len, &rtp))
    {
        return -EINVAL;
    }

    if (have_seq)
    {
        const int16_t diff = static_cast<int16_t>(rtp.seq - last_seq);
        if (diff <= 0)
        {
            rtp_reordered_count++;
            return pop_completed_au(au_out);
        }
    }

    if (have_building_ts && rtp.ts != building_ts)
    {
        finish_building_au(building_ts, building_capture_rt_ns);
    }

    note_sequence(rtp.seq);

    begin_au_if_needed(rtp.ts);

    if (rtp.capture_rt_ns > 0)
    {
        building_capture_rt_ns = rtp.capture_rt_ns;
    }

    const int pr = process_payload(rtp);
    if (pr < 0)
    {
        return pr;
    }

    if (rtp.marker)
    {
        finish_building_au(rtp.ts, building_capture_rt_ns);
    }

    return pop_completed_au(au_out);
}

}  // namespace vstreamer
