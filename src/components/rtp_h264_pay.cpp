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

}  // namespace

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
    opened = true;
    return 0;
}

void rtp_h264_pay::close()
{
    std::lock_guard<std::mutex> lock(mu);
    pending.clear();
    packer.reset();
    opened = false;
}

int rtp_h264_pay::input(uint8_t port, const data_packet &in)
{
    if (0 != port)
    {
        return -EINVAL;
    }
    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::H264)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    const int64_t capture_rt = mono_to_realtime_ns(f.capture_mono_ns);
    if (packer.pack_annexb(f.buf.u8(), f.buf.size(), f.pts, capture_rt) < 0)
    {
        return -EINVAL;
    }
    while (packer.pending())
    {
        std::vector<uint8_t> buf(static_cast<size_t>(mtu));
        const int            n = packer.pop_datagram(buf.data(), buf.size());
        if (n < 0)
        {
            break;
        }
        buf.resize(static_cast<size_t>(n));
        pending_datagram item;
        item.bytes = std::move(buf);
        item.pts = f.pts;
        pending.push_back(std::move(item));
        while (pending.size() > k_pending_cap)
        {
            pending.pop_front();
            datagrams_dropped++;
        }
    }
    return 0;
}

int rtp_h264_pay::output(uint8_t port, data_packet &out, int /*timeout_ms*/)
{
    if (0 != port)
    {
        return -EINVAL;
    }

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
    auto sd = std::make_unique<sock_data>();
    sd->pts = front.pts;
    sd->buf = std::move(buf);
    out.reset(std::move(sd));
    pending.pop_front();
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
