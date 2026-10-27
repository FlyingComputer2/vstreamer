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
    pending_dgrams = 0;
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
    const int64_t capture_rt = mono_to_realtime_ns(static_cast<int64_t>(in.ts_us) * 1000LL);
    const uint32_t rtp_ts =
        static_cast<uint32_t>((static_cast<uint64_t>(in.ts_us) * 90ULL) / 1000ULL);
    if (packer.plan_annexb_rtp_ts(in.sdu.u8(), in.sdu.size(), capture_rt, &plan_scratch) < 0 ||
        plan_scratch.empty())
    {
        return -EINVAL;
    }
    if (pending_dgrams + plan_scratch.size() > k_pending_cap)
    {
        return -EAGAIN;
    }

    pending_au a;
    a.ts_us = in.ts_us;
    a.rtp_ts = rtp_ts;
    a.capture_rt = capture_rt;
    a.dgrams.assign(plan_scratch.begin(), plan_scratch.end());
    /* The packer's SPS/PPS cache changes with later AUs: keep this AU's injected copies. */
    size_t param_bytes = 0;
    for (const rtp_h264_dgram &d : a.dgrams)
    {
        param_bytes += d.cached_param ? static_cast<size_t>(d.payload_len) : 0;
    }
    a.params.reserve(param_bytes);
    for (rtp_h264_dgram &d : a.dgrams)
    {
        if (d.cached_param)
        {
            const size_t at = a.params.size();
            a.params.insert(a.params.end(), d.payload, d.payload + d.payload_len);
            d.payload = a.params.data() + at;
        }
    }
    a.au = std::move(in.sdu);
    pending_dgrams += a.dgrams.size();
    pending.push_back(std::move(a));
    return 0;
}

int rtp_h264_pay::input(component_pdu &&in)
{
    std::lock_guard<std::mutex> lock(mu);
    return input_pdu_locked(std::move(in));
}


int rtp_h264_pay::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu);
    /* The caller's previous datagram may be a view of the bytes the next header goes over. */
    out.sdu = {};
    if (pending.empty())
    {
        return -EAGAIN;
    }

    pending_au           &a = pending.front();
    const rtp_h264_dgram &d = a.dgrams[a.next];
    const size_t          hdr =
        static_cast<size_t>(rtp_h264_packer::rtp_header_len(a.capture_rt) + d.fu_len);
    const size_t total = hdr + static_cast<size_t>(d.payload_len);
    bfc::shared_sized_buffer buf;
    if (!d.cached_param && 1 == a.au.use_count())
    {
        /* Bytes in front of this payload are earlier datagrams' (gone) or start codes. */
        const size_t off = static_cast<size_t>(d.payload - a.au.u8());
        if (off >= hdr)
        {
            buf = a.au.subview(off - hdr, total);
        }
    }
    if (buf.size() == total)
    {
        ++datagrams_in_place;
    }
    else
    {
        buf = pool.acquire(total);
        if (0 == buf.capacity() || buf.size() != total)
        {
            return -ENOMEM;
        }
        std::memcpy(buf.u8() + hdr, d.payload, static_cast<size_t>(d.payload_len));
        ++datagrams_copied;
    }
    packer.write_rtp_header(buf.u8(), d.marker, a.rtp_ts, a.capture_rt);
    if (d.fu_len > 0)
    {
        std::memcpy(buf.u8() + hdr - static_cast<size_t>(d.fu_len), d.fu,
                    static_cast<size_t>(d.fu_len));
    }

    out.ts_us = a.ts_us;
    out.seq = out_seq_++;
    out.sdu_type = sdu_type_e::RTP;
    out.port = 0;
    out.flags = d.marker ? static_cast<uint8_t>(pdu_flag_e::AU_END) : 0;
    out.sdu = std::move(buf);
    --pending_dgrams;
    if (++a.next == a.dgrams.size())
    {
        pending.pop_front();
    }
    notify_wakeup();
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
    if ("datagrams_in_place" == key || "datagrams_copied" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64,
                      "datagrams_in_place" == key ? datagrams_in_place : datagrams_copied);
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
