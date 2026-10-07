#include "components/rtp_h264_depay.hpp"

#include "core/h264_sps.hpp"
#include "core/key_util.hpp"
#include "core/time_util.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace vstreamer
{
namespace
{

constexpr int64_t k_capture_skew_min_ns = -50'000'000LL;
constexpr int64_t k_capture_skew_max_ns = 60'000'000'000LL;

}  // namespace

const std::vector<port_desc> &rtp_h264_depay::input_ports()
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

const std::vector<port_desc> &rtp_h264_depay::output_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry coded {};
        coded.sdu_type = sdu_type_e::H264_AU;
        p.caps.push_back(coded);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

rtp_h264_depay::rtp_h264_depay() = default;

rtp_h264_depay::~rtp_h264_depay()
{
    close();
}

std::string rtp_h264_depay::name() const
{
    return "rtp_h264_depay";
}

media_kind_e rtp_h264_depay::input_kind() const
{
    return media_kind_e::UNKNOWN;
}

media_kind_e rtp_h264_depay::output_kind() const
{
    return media_kind_e::H264;
}

packet_kind_e rtp_h264_depay::input_packet_kind() const
{
    return packet_kind_e::SOCK;
}

packet_kind_e rtp_h264_depay::output_packet_kind() const
{
    return packet_kind_e::FRAME;
}

int rtp_h264_depay::open()
{
    std::lock_guard<std::mutex> lock(mu);
    depay = rtp_h264_depacketizer(fps);
    out_queue.clear();
    au_dropped = 0;
    capture_ts_rejected = 0;
    capture_skew_ms = 0.0;
    last_caps_w_ = 0;
    last_caps_h_ = 0;
    out_seq_ = 0;
    opened = true;
    return 0;
}

void rtp_h264_depay::close()
{
    std::lock_guard<std::mutex> lock(mu);
    depay.reset();
    out_queue.clear();
    opened = false;
}

int64_t rtp_h264_depay::accept_capture_rt_ns(int64_t capture_rt_ns)
{
    if (capture_rt_ns <= 0)
    {
        return 0;
    }
    const int64_t now_mono = steady_mono_ns();
    const int64_t now_rt = realtime_ns();
    const int64_t skew_ns = now_rt - capture_rt_ns;
    if (skew_ns < k_capture_skew_min_ns || skew_ns > k_capture_skew_max_ns)
    {
        capture_ts_rejected++;
        return 0;
    }
    capture_skew_ms = static_cast<double>(skew_ns) / 1e6;
    return now_mono - skew_ns;
}

void rtp_h264_depay::maybe_queue_caps_for_au(const au_item &item)
{
    int32_t w = 0;
    int32_t h = 0;
    if (!h264_annexb_sps_dimensions(item.buf.data(), item.buf.size(), &w, &h))
    {
        w = 0;
        h = 0;
    }
    if (w == last_caps_w_ && h == last_caps_h_)
    {
        return;
    }
    last_caps_w_ = w;
    last_caps_h_ = h;
    video_coded_caps caps {};
    caps.width = w;
    caps.height = h;
    component_pdu caps_pdu = make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, caps, item.ts_us, 0);
    caps_pdu.seq = 0;
    out_queue.push_back(std::move(caps_pdu));
}

void rtp_h264_depay::push_au(au_item &&item)
{
    if (out_queue.size() >= k_au_queue_depth * 2)
    {
        au_dropped++;
        return;
    }
    maybe_queue_caps_for_au(item);
    component_pdu pdu;
    pdu.ts_us = item.ts_us;
    pdu.seq = out_seq_++;
    pdu.sdu_type = sdu_type_e::H264_AU;
    pdu.port = 0;
    pdu.flags = 0;
    if (item.key)
    {
        pdu.flags |= static_cast<uint8_t>(pdu_flag_e::KEY);
    }
    pdu.sdu = shared_sized_buffer::copy_from(item.buf.data(), item.buf.size());
    if (pdu.sdu.empty() && !item.buf.empty())
    {
        au_dropped++;
        return;
    }
    out_queue.push_back(std::move(pdu));
    notify_wakeup();
}

int rtp_h264_depay::input(component_pdu &&in)
{
    if (0 != in.port || in.sdu_type != sdu_type_e::RTP)
    {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> lock(mu);
    const shared_sized_buffer &feed = in.sdu;
    const uint64_t             rx_ts_us = in.ts_us;
    for (;;)
    {
        std::vector<uint8_t> au;
        const int            ready = depay.feed(feed.u8(), feed.size(), &au);
        if (ready < 0)
        {
            return ready;
        }
        if (0 == ready)
        {
            break;
        }
        au_item item;
        item.buf = std::move(au);
        const int64_t cap_mono = accept_capture_rt_ns(depay.au_capture_rt_ns());
        if (cap_mono > 0)
        {
            item.ts_us = static_cast<uint64_t>(cap_mono / 1000LL);
        }
        else
        {
            item.ts_us = rx_ts_us;
        }
        item.key = depay.au_key();
        push_au(std::move(item));
    }
    return 0;
}

int rtp_h264_depay::input(uint8_t port, const data_packet &in)
{
    if (0 != port || in.get_type() != packet_kind_e::SOCK)
    {
        return -EINVAL;
    }
    const sock_data &s = data_packet::cast<sock_data>(in);
    component_pdu pdu;
    pdu.ts_us = s.pts > 0 ? static_cast<uint64_t>(s.pts) : 0ULL;
    pdu.sdu_type = sdu_type_e::RTP;
    pdu.port = 0;
    pdu.sdu = s.buf;
    return input(std::move(pdu));
}

int rtp_h264_depay::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu);
    if (out_queue.empty())
    {
        return -EAGAIN;
    }
    out = std::move(out_queue.front());
    out_queue.pop_front();
    return 0;
}

int rtp_h264_depay::output(uint8_t port, data_packet &out, int timeout_ms)
{
    (void)timeout_ms;
    if (0 != port)
    {
        return -EINVAL;
    }
    for (;;)
    {
        component_pdu pdu;
        const int     r = rtp_h264_depay::output(pdu);
        if (0 != r)
        {
            return r;
        }
        if (is_caps(pdu.sdu_type))
        {
            continue;
        }
        auto fd = std::make_unique<frame_data>();
        fd->kind = media_kind_e::H264;
        fd->width = last_caps_w_;
        fd->height = last_caps_h_;
        fd->capture_mono_ns = static_cast<int64_t>(pdu.ts_us) * 1000LL;
        fd->pts = fd->capture_mono_ns;
        fd->key = has_flag(pdu, pdu_flag_e::KEY);
        fd->buf = std::move(pdu.sdu);
        out.reset(std::move(fd));
        return 0;
    }
}

int rtp_h264_depay::configure(std::string_view key, std::string_view value)
{
    if ("fps" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 1 || v > 120 || value.size() > 31)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu);
        fps = static_cast<int>(v);
        if (opened)
        {
            depay = rtp_h264_depacketizer(fps);
        }
        return 0;
    }
    return -ENOTSUP;
}

int rtp_h264_depay::query(std::string_view key, std::string *value) const
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

    if ("loss" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(depay.packet_loss()));
        *value = buf;
        return 0;
    }
    if ("need_idr" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, depay.need_idr());
        *value = buf;
        return 0;
    }
    if ("nal_dropped" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, depay.nal_dropped());
        *value = buf;
        return 0;
    }
    if ("rtp_reordered" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, depay.rtp_reordered());
        *value = buf;
        return 0;
    }
    if ("au_dropped" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, au_dropped);
        *value = buf;
        return 0;
    }
    if ("capture_ts_rejected" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, capture_ts_rejected);
        *value = buf;
        return 0;
    }
    if ("capture_skew_ms" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", capture_skew_ms);
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
