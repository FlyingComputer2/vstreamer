#include "components/rtp_h264_depay.hpp"

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
    au_queue.clear();
    au_dropped = 0;
    capture_ts_rejected = 0;
    capture_skew_ms = 0.0;
    opened = true;
    return 0;
}

void rtp_h264_depay::close()
{
    std::lock_guard<std::mutex> lock(mu);
    depay.reset();
    au_queue.clear();
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

void rtp_h264_depay::push_au(au_item &&item)
{
    if (au_queue.size() >= k_au_queue_depth)
    {
        au_queue.pop_front();
        au_dropped++;
    }
    au_queue.push_back(std::move(item));
}

int rtp_h264_depay::input(uint8_t port, const data_packet &in)
{
    if (0 != port)
    {
        return -EINVAL;
    }
    const sock_data &s = data_packet::cast<sock_data>(in);
    std::lock_guard<std::mutex> lock(mu);
    const shared_sized_buffer &feed = s.buf;
    for (;;)
    {
        const int64_t        feed_in_ns = steady_mono_ns();
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
        const int64_t feed_out_ns = steady_mono_ns();
        if (feed_out_ns > feed_in_ns)
        {
            last_node_latency_ms =
                static_cast<double>(feed_out_ns - feed_in_ns) / 1e6;
        }
        au_item item;
        item.buf = std::move(au);
        item.pts = depay.au_pts();
        item.capture_mono_ns = accept_capture_rt_ns(depay.au_capture_rt_ns());
        item.key = depay.au_key();
        push_au(std::move(item));
    }
    return 0;
}

int rtp_h264_depay::output(uint8_t port, data_packet &out, int /*timeout_ms*/)
{
    if (0 != port)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (au_queue.empty())
    {
        return -EAGAIN;
    }

    au_item item = std::move(au_queue.front());
    au_queue.pop_front();

    auto fd = std::make_unique<frame_data>();
    fd->kind = media_kind_e::H264;
    fd->width = 0;
    fd->height = 0;
    fd->pts = item.pts;
    fd->capture_mono_ns = item.capture_mono_ns;
    fd->key = item.key;
    fd->buf = shared_sized_buffer::copy_from(item.buf.data(), item.buf.size());
    if (fd->buf.empty() && !item.buf.empty())
    {
        return -ENOMEM;
    }
    out.reset(std::move(fd));
    return 0;
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
    if ("node_latency_ms" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        if (std::snprintf(buf, sizeof(buf), "%.2f", last_node_latency_ms) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
