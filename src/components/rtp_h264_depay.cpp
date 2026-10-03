#include "components/rtp_h264_depay.hpp"

#include "core/key_util.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace vstreamer
{

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
        item.pts = depay.au_pts();
        item.capture_mono_ns = depay.au_capture_mono_ns();
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
    if ("capture_epoch_ns" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || value.size() > 31)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu);
        depay.set_capture_epoch_ns(v);
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
    return -ENOTSUP;
}

}  // namespace vstreamer
