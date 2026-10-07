#include "components/rtp_h264_pay.hpp"

#include "core/key_util.hpp"
#include "core/time_util.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstring>

namespace vstreamer
{
namespace
{

constexpr int k_rtp_header_len = 12;
constexpr int k_capture_ext_len = 16;
constexpr int k_fu_a_header_len = 2;
constexpr int k_min_mtu =
    k_rtp_header_len + k_capture_ext_len + k_fu_a_header_len + 1;
constexpr int k_max_mtu = 65507;

[[nodiscard]] bool rtp_marker(const std::vector<uint8_t> &dg)
{
    return dg.size() >= 2 && (dg[1] & 0x80) != 0;
}

}  // namespace

const std::vector<port_desc> &rtp_h264_pay::input_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::H264_AU;
        p.caps.push_back(caps);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

const std::vector<port_desc> &rtp_h264_pay::output_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::RTP;
        p.caps.push_back(caps);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

rtp_h264_pay::rtp_h264_pay() : pool(1400, 64) {}

void rtp_h264_pay::recreate_pool_locked()
{
    pool = buffer_pool(static_cast<size_t>(mtu), 64);
}

rtp_h264_pay::~rtp_h264_pay()
{
    close();
}

std::string rtp_h264_pay::name() const
{
    return "rtp_h264_pay";
}

media_kind_e rtp_h264_pay::input_kind() const
{
    return media_kind_e::H264;
}

media_kind_e rtp_h264_pay::output_kind() const
{
    return media_kind_e::UNKNOWN;
}

packet_kind_e rtp_h264_pay::input_packet_kind() const
{
    return packet_kind_e::FRAME;
}

packet_kind_e rtp_h264_pay::output_packet_kind() const
{
    return packet_kind_e::SOCK;
}

void rtp_h264_pay::rebuild_packer()
{
    rtp_h264_config cfg;
    cfg.mtu = mtu;
    cfg.payload_type = pt;
    cfg.ssrc = ssrc;
    cfg.fps = fps;
    packer = rtp_h264_packer(cfg);
}

int rtp_h264_pay::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    recreate_pool_locked();
    rebuild_packer();
    have_coded_caps_ = false;
    out_seq_ = 0;
    opened = true;
    return 0;
}

void rtp_h264_pay::close()
{
    std::lock_guard<std::mutex> lock(mu);
    pending.clear();
    packer.reset();
    have_coded_caps_ = false;
    opened = false;
}

int rtp_h264_pay::input_pdu_locked(component_pdu &&in)
{
    if (0 != in.port)
    {
        return -EINVAL;
    }
    if (in.sdu_type == sdu_type_e::CAPS_VIDEO_CODED)
    {
        video_coded_caps caps {};
        if (read_caps(in, &caps) != 0)
        {
            return -EINVAL;
        }
        coded_caps_ = caps;
        have_coded_caps_ = true;
        return 0;
    }
    if (in.sdu_type != sdu_type_e::H264_AU)
    {
        return -EINVAL;
    }
    if (!have_coded_caps_)
    {
        return -ENOTSUP;
    }
    if (pending.size() >= k_pending_cap)
    {
        return -EAGAIN;
    }

    const int64_t capture_rt = mono_to_realtime_ns(static_cast<int64_t>(in.ts_us) * 1000LL);
    const int64_t pts = static_cast<int64_t>(in.ts_us);
    if (packer.pack_annexb(in.sdu.u8(), in.sdu.size(), pts, capture_rt) < 0)
    {
        return -EINVAL;
    }
    while (packer.pending())
    {
        if (pending.size() >= k_pending_cap)
        {
            return -EAGAIN;
        }
        std::vector<uint8_t> buf(static_cast<size_t>(mtu));
        const int            n = packer.pop_datagram(buf.data(), buf.size());
        if (n < 0)
        {
            break;
        }
        buf.resize(static_cast<size_t>(n));
        pending_datagram item;
        item.bytes = std::move(buf);
        item.ts_us = in.ts_us;
        item.au_end = rtp_marker(item.bytes);
        pending.push_back(std::move(item));
    }
    return 0;
}

int rtp_h264_pay::input(component_pdu &&in)
{
    std::lock_guard<std::mutex> lock(mu);
    return input_pdu_locked(std::move(in));
}

int rtp_h264_pay::input(uint8_t port, const data_packet &in)
{
    if (0 != port || in.get_type() != packet_kind_e::FRAME)
    {
        return -EINVAL;
    }
    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::H264)
    {
        return -EINVAL;
    }
    component_pdu pdu;
    pdu.ts_us = f.capture_mono_ns > 0 ? static_cast<uint64_t>(f.capture_mono_ns / 1000LL)
                                      : static_cast<uint64_t>(f.pts);
    pdu.sdu_type = sdu_type_e::H264_AU;
    pdu.port = 0;
    pdu.sdu = f.buf;
    if (f.key)
    {
        pdu.flags |= static_cast<uint8_t>(pdu_flag_e::KEY);
    }
    std::lock_guard<std::mutex> lock(mu);
    video_coded_caps caps {};
    caps.width = f.width;
    caps.height = f.height;
    component_pdu caps_pdu = make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, caps, pdu.ts_us, 0);
    const int     cr = input_pdu_locked(std::move(caps_pdu));
    if (cr < 0)
    {
        return cr;
    }
    return input_pdu_locked(std::move(pdu));
}

int rtp_h264_pay::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu);
    if (pending.empty())
    {
        return -EAGAIN;
    }

    const pending_datagram &front = pending.front();
    shared_sized_buffer     buf = pool.acquire(front.bytes.size());
    if (0 == buf.capacity() || buf.size() != front.bytes.size())
    {
        return -ENOMEM;
    }
    std::memcpy(buf.u8(), front.bytes.data(), front.bytes.size());
    out.ts_us = front.ts_us;
    out.seq = out_seq_++;
    out.sdu_type = sdu_type_e::RTP;
    out.port = 0;
    out.flags = 0;
    if (front.au_end)
    {
        out.flags |= static_cast<uint8_t>(pdu_flag_e::AU_END);
    }
    out.sdu = std::move(buf);
    pending.pop_front();
    notify_wakeup();
    return 0;
}

int rtp_h264_pay::output(uint8_t port, data_packet &out, int /*timeout_ms*/)
{
    if (0 != port)
    {
        return -EINVAL;
    }
    component_pdu pdu;
    const int     r = output(pdu);
    if (0 != r)
    {
        return r;
    }
    auto sd = std::make_unique<sock_data>();
    sd->pts = static_cast<int64_t>(pdu.ts_us);
    sd->buf = std::move(pdu.sdu);
    out.reset(std::move(sd));
    return 0;
}

int rtp_h264_pay::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(mu);
    if ("mtu" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || value.size() > 31)
        {
            return -EINVAL;
        }
        if (v < k_min_mtu || v > k_max_mtu)
        {
            return -EINVAL;
        }
        mtu = static_cast<int>(v);
        recreate_pool_locked();
        if (opened)
        {
            rebuild_packer();
        }
        return 0;
    }
    if ("fps" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 1 || v > 120 || value.size() > 31)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(v);
        if (opened)
        {
            rebuild_packer();
        }
        return 0;
    }
    return -ENOTSUP;
}

int rtp_h264_pay::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    int r = port_caps_query(input_ports(), true, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }
    r = port_caps_query(output_ports(), false, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }
    if ("datagrams_dropped" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, datagrams_dropped);
        *value = buf;
        return 0;
    }
    if ("pool_misses" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, pool.misses());
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
