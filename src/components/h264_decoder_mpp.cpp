#include "components/h264_decoder_mpp.hpp"

#include "core/key_util.hpp"
#include "core/sdu_type.hpp"
#include "core/output_opts.hpp"
#include "core/pix_convert.hpp"
#include "core/time_util.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#define MODULE_TAG "vstreamer_mpp_h264"

extern "C"
{
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_mpi_cmd.h>
#include <rockchip/rk_vdec_cfg.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
}

namespace vstreamer
{
namespace
{

constexpr size_t k_max_au = 8ULL * 1024ULL * 1024ULL;
constexpr int    k_frm_grp_count = 24;

int parse_size(std::string_view s, int *w, int *h)
{
    if (nullptr == w || nullptr == h || s.empty())
    {
        return -EINVAL;
    }
    char buf[64];
    if (s.size() >= sizeof(buf))
    {
        return -EINVAL;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    char *x = std::strchr(buf, 'x');
    if (nullptr == x)
    {
        x = std::strchr(buf, 'X');
    }
    if (nullptr == x || x == buf || x[1] == '\0')
    {
        return -EINVAL;
    }
    *x = '\0';
    int64_t ww = 0;
    int64_t hh = 0;
    if (key_parse_i64(buf, &ww) < 0 || key_parse_i64(x + 1, &hh) < 0)
    {
        return -EINVAL;
    }
    if (ww < 2 || hh < 2 || (ww % 2) || (hh % 2) || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

}  // namespace

const std::vector<port_desc> &h264_decoder_mpp::input_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc       p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_CODED;
        p.caps.push_back(caps);
        port_caps_entry data {};
        data.sdu_type = sdu_type_e::H264_AU;
        p.caps.push_back(data);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

const std::vector<port_desc> &h264_decoder_mpp::output_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc       p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_RAW;
        p.caps.push_back(caps);
        port_caps_entry data {};
        data.sdu_type = sdu_type_e::NV12;
        p.caps.push_back(data);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

bool h264_decoder_mpp::coded_caps_acceptable(const video_coded_caps &caps) const
{
    if (!output_size_stream)
    {
        if (caps.width != width || caps.height != height)
        {
            return false;
        }
    }
    else if (caps.width <= 0 || caps.height <= 0)
    {
        return width > 0 && height > 0;
    }
    for (const port_caps_entry &entry : input_ports()[0].caps)
    {
        if (entry.sdu_type == sdu_type_e::CAPS_VIDEO_CODED && match(entry, caps))
        {
            return true;
        }
    }
    return false;
}

void h264_decoder_mpp::maybe_emit_output_caps_locked(int32_t w, int32_t h, int32_t hor,
                                                     int32_t ver, uint64_t ts_us)
{
    video_raw_caps raw {};
    raw.width = w;
    raw.height = h;
    raw.hor_stride = hor;
    raw.ver_stride = ver;
    if (have_output_caps_ && raw.width == output_caps_.width && raw.height == output_caps_.height &&
        raw.hor_stride == output_caps_.hor_stride && raw.ver_stride == output_caps_.ver_stride)
    {
        return;
    }
    output_caps_ = raw;
    have_output_caps_ = true;
    component_pdu caps_pdu = make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, raw, ts_us, 0);
    caps_pdu.seq = 0;
    pending_caps_out_.push_back(std::move(caps_pdu));
}

void h264_decoder_mpp::drain_thread_main()
{
    while (!drain_stop_.load(std::memory_order_relaxed))
    {
        if (!opened)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        drain_mpp_to_ready(10);
        notify_wakeup();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

h264_decoder_mpp::h264_decoder_mpp() = default;

h264_decoder_mpp::~h264_decoder_mpp()
{
    close();
}

std::string h264_decoder_mpp::name() const
{
    return "h264_decoder_mpp";
}



int h264_decoder_mpp::ensure_decoder_locked()
{
    if (nullptr != ctx)
    {
        return 0;
    }

    MppCtx  mpp_ctx = nullptr;
    MppApi *mpp_mpi = nullptr;
    MPP_RET ret = mpp_create(&mpp_ctx, &mpp_mpi);
    if (ret != MPP_OK || nullptr == mpp_ctx || nullptr == mpp_mpi)
    {
        return -EIO;
    }

    ret = mpp_init(mpp_ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK)
    {
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    MppDecCfg cfg = nullptr;
    mpp_dec_cfg_init(&cfg);
    if (nullptr == cfg)
    {
        mpp_destroy(mpp_ctx);
        return -ENOMEM;
    }

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_GET_CFG, cfg);
    if (ret != MPP_OK)
    {
        mpp_dec_cfg_deinit(cfg);
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    /* Split Annex-B byte stream into access units inside MPP. */
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
    if (ret != MPP_OK)
    {
        mpp_dec_cfg_deinit(cfg);
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_CFG, cfg);
    mpp_dec_cfg_deinit(cfg);
    if (ret != MPP_OK)
    {
        mpp_destroy(mpp_ctx);
        return -EIO;
    }

    ctx = mpp_ctx;
    mpi = mpp_mpi;
    return 0;
}

void h264_decoder_mpp::free_decoder_locked()
{
    if (ctx)
    {
        auto *mpp_ctx = static_cast<MppCtx>(ctx);
        auto *mpp_mpi = static_cast<MppApi *>(mpi);
        if (mpp_mpi)
        {
            for (int i = 0; i < 64; i++)
            {
                MppFrame frm = nullptr;
                const MPP_RET gr = mpp_mpi->decode_get_frame(mpp_ctx, &frm);
                if (gr != MPP_OK || nullptr == frm)
                {
                    break;
                }
                mpp_frame_deinit(&frm);
            }
            mpp_mpi->reset(mpp_ctx);
        }
        mpp_destroy(mpp_ctx);
        ctx = nullptr;
        mpi = nullptr;
    }
    if (frm_grp)
    {
        MppBufferGroup grp = static_cast<MppBufferGroup>(frm_grp);
        mpp_buffer_group_put(grp);
        frm_grp = nullptr;
    }
    nv12_pool.reset();
    nv12_pool_bytes = 0;
}

void h264_decoder_mpp::clear_pending_locked()
{
    ready_frames.clear();
    pending_caps_out_.clear();
}

int h264_decoder_mpp::handle_info_change_locked(void *mpp_frame)
{
    auto *frame = static_cast<MppFrame>(mpp_frame);
    auto *mpp_ctx = static_cast<MppCtx>(ctx);
    auto *mpp_mpi = static_cast<MppApi *>(mpi);

    RK_U32 buf_size = mpp_frame_get_buf_size(frame);
    if (buf_size == 0)
    {
        return -EIO;
    }

    if (frm_grp)
    {
        MppBufferGroup old = static_cast<MppBufferGroup>(frm_grp);
        mpp_buffer_group_put(old);
        frm_grp = nullptr;
    }

    MppBufferGroup grp = nullptr;
    MPP_RET        ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK || nullptr == grp)
    {
        return -ENOMEM;
    }
    ret = mpp_buffer_group_limit_config(grp, buf_size, k_frm_grp_count);
    if (ret != MPP_OK)
    {
        mpp_buffer_group_put(grp);
        return -EIO;
    }

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
    if (ret != MPP_OK)
    {
        mpp_buffer_group_put(grp);
        return -EIO;
    }
    frm_grp = grp;

    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
    if (ret != MPP_OK)
    {
        return -EIO;
    }
    return 0;
}

int h264_decoder_mpp::pack_mpp_to_ready_locked(void *mpp_frame)
{
    auto *frame = static_cast<MppFrame>(mpp_frame);
    if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame))
    {
        if (log_errinfo_throttle++ < 5)
        {
            std::fprintf(stderr,
                         "h264_decoder_mpp: frame errinfo=%u discard=%u\n",
                         mpp_frame_get_errinfo(frame), mpp_frame_get_discard(frame));
        }
        return -EIO;
    }

    MppBuffer buffer = mpp_frame_get_buffer(frame);
    if (nullptr == buffer)
    {
        return -EIO;
    }

    if (output_format != sdu_type_e::NV12)
    {
        return -ENOTSUP;
    }

    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    MppFrameFormat raw = static_cast<MppFrameFormat>(fmt & MPP_FRAME_FMT_MASK);
    if (MPP_FRAME_FMT_IS_FBC(fmt) || MPP_FRAME_FMT_IS_TILE(fmt))
    {
        if (log_fbc_throttle++ < 5)
        {
            std::fprintf(stderr, "h264_decoder_mpp: unsupported MPP fmt=0x%x (fbc/tile)\n",
                         static_cast<unsigned>(fmt));
        }
        return -ENOTSUP;
    }

    const bool is_420 = (raw == MPP_FMT_YUV420SP || raw == MPP_FMT_YUV420SP_VU);
    const bool is_422 = (raw == MPP_FMT_YUV422SP || raw == MPP_FMT_YUV422SP_VU);
    if (!is_420 && !(is_422 && output_mode == output_mode_e::convert))
    {
        if (log_pix_throttle++ < 5)
        {
            std::fprintf(stderr, "h264_decoder_mpp: unsupported pixel raw=0x%x mode=%d\n",
                         static_cast<unsigned>(raw), static_cast<int>(output_mode));
        }
        return -ENOTSUP;
    }

    auto *base = static_cast<const uint8_t *>(mpp_buffer_get_ptr(buffer));
    if (nullptr == base)
    {
        return -EIO;
    }

    int src_w = static_cast<int>(mpp_frame_get_width(frame));
    int src_h = static_cast<int>(mpp_frame_get_height(frame));
    int hor = static_cast<int>(mpp_frame_get_hor_stride(frame));
    int ver = static_cast<int>(mpp_frame_get_ver_stride(frame));

    const bool swap_chroma =
        (raw == MPP_FMT_YUV420SP_VU || raw == MPP_FMT_YUV422SP_VU);
    const int  out_w = output_size_stream ? src_w : width;
    const int  out_h = output_size_stream ? src_h : height;
    const size_t nv12_sz =
        static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 3ULL / 2ULL;
    if (!nv12_pool || nv12_pool_bytes != nv12_sz)
    {
        nv12_pool = std::make_unique<buffer_pool>(nv12_sz, k_max_ready_frames);
        nv12_pool_bytes = nv12_sz;
    }
    shared_sized_buffer out_buf = nv12_pool->acquire(nv12_sz);
    if (out_buf.empty())
    {
        return -ENOMEM;
    }

    int r = 0;
    if (is_420)
    {
        r = pack_yuv420sp_to_nv12(base, src_w, src_h, hor, ver, out_buf.u8(), out_w, out_h,
                                  swap_chroma);
    }
    else
    {
        r = pack_yuv422sp_to_nv12(base, src_w, src_h, hor, ver, out_buf.u8(), out_w, out_h,
                                  swap_chroma);
    }
    if (r < 0)
    {
        return r;
    }

    const uint64_t ts_us = static_cast<uint64_t>(mpp_frame_get_pts(frame));
    maybe_emit_output_caps_locked(out_w, out_h, hor, ver, ts_us);
    component_pdu pdu;
    pdu.ts_us = ts_us;
    pdu.seq = 0;
    pdu.sdu_type = sdu_type_e::NV12;
    pdu.port = 0;
    pdu.flags = 0;
    pdu.sdu = std::move(out_buf);
    if (ts_us > 0)
    {
        const int64_t now_ns = steady_mono_ns();
        const int64_t cap_ns = static_cast<int64_t>(ts_us) * 1000LL;
        const double  ms = static_cast<double>(now_ns - cap_ns) / 1e6;
        if (ms >= 0.0)
        {
            last_latency_ms = ms;
        }
    }
    ready_frames.push_back(std::move(pdu));
    notify_wakeup();
    return 0;
}

void h264_decoder_mpp::drain_mpp_to_ready(int timeout_ms)
{
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (ready_frames.size() >= k_max_ready_frames)
            {
                return;
            }
        }
        const int r = fetch_one_mpp_frame(timeout_ms);
        if (r < 0)
        {
            return;
        }
    }
}

int h264_decoder_mpp::fetch_one_mpp_frame(int timeout_ms)
{
    if (ready_frames.size() >= k_max_ready_frames)
    {
        return -EAGAIN;
    }

    void *mpp_ctx = nullptr;
    void *mpp_mpi = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (nullptr == ctx || nullptr == mpi)
        {
            return -EBADF;
        }
        mpp_ctx = ctx;
        mpp_mpi = mpi;
    }

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now();
    if (timeout_ms > 0)
    {
        deadline += std::chrono::milliseconds(timeout_ms);
    }
    else if (timeout_ms < 0)
    {
        deadline = clock::time_point::max();
    }

    for (;;)
    {
        if (cancel_io.load())
        {
            return -ECANCELED;
        }
        MppFrame frame = nullptr;
        MPP_RET  ret = MPP_OK;
        {
            std::lock_guard<std::mutex> api_lock(mpp_io_mu);
            ret = static_cast<MppApi *>(mpp_mpi)->decode_get_frame(static_cast<MppCtx>(mpp_ctx),
                                                                   &frame);
        }
        if (ret == MPP_ERR_TIMEOUT || (ret == MPP_OK && nullptr == frame))
        {
            if (timeout_ms == 0)
            {
                return -EAGAIN;
            }
            if (timeout_ms > 0 && clock::now() >= deadline)
            {
                return -EAGAIN;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (ret != MPP_OK)
        {
            return -EIO;
        }

        if (mpp_frame_get_info_change(frame))
        {
            int r = 0;
            {
                std::lock_guard<std::mutex> lock(mu);
                r = handle_info_change_locked(frame);
            }
            mpp_frame_deinit(&frame);
            if (r < 0)
            {
                return r;
            }
            continue;
        }

        int r = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            r = pack_mpp_to_ready_locked(frame);
        }
        mpp_frame_deinit(&frame);
        return r;
    }
}

int h264_decoder_mpp::open()
{
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return 0;
        }
        const int r = ensure_decoder_locked();
        if (r < 0)
        {
            return r;
        }
        opened = true;
        cancel_io = false;
    }
    drain_stop_.store(false, std::memory_order_relaxed);
    if (!drain_thread_.joinable())
    {
        drain_thread_ = std::thread([this]() { drain_thread_main(); });
    }
    return 0;
}

void h264_decoder_mpp::cancel_pending_io()
{
    cancel_io = true;
}

void h264_decoder_mpp::close()
{
    cancel_pending_io();
    drain_stop_.store(true, std::memory_order_relaxed);
    notify_wakeup();
    if (drain_thread_.joinable())
    {
        drain_thread_.join();
    }
    std::lock_guard<std::mutex> lock(mu);
    clear_pending_locked();
    free_decoder_locked();
    opened = false;
    cancel_io = false;
    have_input_caps_ = false;
    caps_reject_ = false;
    have_output_caps_ = false;
    pending_caps_out_.clear();
}

int h264_decoder_mpp::input_pdu_locked(component_pdu &&in)
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
        video_coded_caps use_caps = caps;
        if (output_size_stream && (use_caps.width <= 0 || use_caps.height <= 0))
        {
            if (width <= 0 || height <= 0)
            {
                caps_reject_ = true;
                have_input_caps_ = false;
                return -ENOTSUP;
            }
            use_caps.width = width;
            use_caps.height = height;
        }
        if (!coded_caps_acceptable(use_caps))
        {
            caps_reject_ = true;
            have_input_caps_ = false;
            return -ENOTSUP;
        }
        caps_reject_ = false;
        input_caps_ = use_caps;
        have_input_caps_ = true;
        if (!output_size_stream)
        {
            width = use_caps.width;
            height = use_caps.height;
        }
        return 0;
    }
    if (in.sdu_type != sdu_type_e::H264_AU)
    {
        return -EINVAL;
    }
    if (!have_input_caps_ && output_size_stream && width > 0 && height > 0)
    {
        video_coded_caps implied {};
        implied.width = width;
        implied.height = height;
        input_caps_ = implied;
        have_input_caps_ = true;
        caps_reject_ = false;
    }
    if (caps_reject_ || !have_input_caps_)
    {
        return -ENOTSUP;
    }
    if (in.sdu.size() > k_max_au)
    {
        return -EINVAL;
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!opened || nullptr == ctx || nullptr == mpi)
        {
            return -EBADF;
        }
        if (ready_frames.size() >= k_max_ready_frames)
        {
            return -EAGAIN;
        }
    }

    MppPacket packet = nullptr;
    MPP_RET   ret = mpp_packet_init(&packet, const_cast<uint8_t *>(in.sdu.u8()), in.sdu.size());
    if (ret != MPP_OK || nullptr == packet)
    {
        return -ENOMEM;
    }
    mpp_packet_set_pts(packet, static_cast<int64_t>(in.ts_us));
    mpp_packet_set_length(packet, in.sdu.size());

    void *mpp_ctx = nullptr;
    void *mpp_mpi = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu);
        mpp_ctx = ctx;
        mpp_mpi = mpi;
    }

    {
        std::lock_guard<std::mutex> api_lock(mpp_io_mu);
        ret = static_cast<MppApi *>(mpp_mpi)->decode_put_packet(static_cast<MppCtx>(mpp_ctx),
                                                                packet);
    }
    mpp_packet_deinit(&packet);

    if (ret == MPP_ERR_BUFFER_FULL)
    {
        return -EAGAIN;
    }
    if (ret != MPP_OK)
    {
        return -EIO;
    }
    return 0;
}

int h264_decoder_mpp::input(component_pdu &&in)
{
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!opened)
        {
            return -EBADF;
        }
    }
    return input_pdu_locked(std::move(in));
}


int h264_decoder_mpp::output(component_pdu &out)
{
    if (!pending_caps_out_.empty())
    {
        out = std::move(pending_caps_out_.front());
        pending_caps_out_.pop_front();
        return 0;
    }
    std::lock_guard<std::mutex> lock(mu);
    if (ready_frames.empty())
    {
        return -EAGAIN;
    }
    out = std::move(ready_frames.front());
    out.seq = out_seq_++;
    ready_frames.pop_front();
    return 0;
}


int h264_decoder_mpp::configure(std::string_view key, std::string_view value)
{
    std::string_view v = value;

    std::lock_guard<std::mutex> lock(mu);
    if (key == "size")
    {
        int w = 0;
        int h = 0;
        int r = parse_size(v, &w, &h);
        if (r < 0)
        {
            return r;
        }
        width = w;
        height = h;
        return 0;
    }
    if (key == "fps")
    {
        int64_t     n = 0;
        std::string tmp(v);
        int         r = key_parse_i64(tmp.c_str(), &n);
        if (r < 0 || n <= 0 || n > 240)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(n);
        return 0;
    }
    if (key == "output_mode")
    {
        output_mode_e mode = output_mode_e::filter;
        int           r = parse_output_mode(v, &mode);
        if (r < 0)
        {
            return r;
        }
        output_mode = mode;
        return 0;
    }
    if (key == "output_format")
    {
        sdu_type_e kind = sdu_type_e::UNKNOWN;
        int        r = parse_output_format(v, &kind);
        if (r < 0)
        {
            return r;
        }
        output_format = kind;
        return 0;
    }
    if (key == "output_size_mode")
    {
        if (v == "stream")
        {
            output_size_stream = true;
            return 0;
        }
        if (v == "config")
        {
            output_size_stream = false;
            return 0;
        }
        return -EINVAL;
    }
    return -ENOTSUP;
}

int h264_decoder_mpp::query(std::string_view key, std::string *value) const
{
    std::lock_guard<std::mutex> lock(mu);
    if (key == "status")
    {
        *value = opened ? "open" : "closed";
        return 0;
    }
    if (key == "size")
    {
        char buf[64];
        if (std::snprintf(buf, sizeof(buf), "%dx%d", width, height) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "fps")
    {
        char buf[32];
        if (key_format_i64(fps, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "format" || key == "output_format")
    {
        const char *name = output_format_name(output_format);
        if (nullptr == name || name[0] == '\0')
        {
            return -EINVAL;
        }
        *value = name;
        return 0;
    }
    if (key == "output_mode")
    {
        *value = output_mode_name(output_mode);
        return 0;
    }
    if (key == "latency_ms")
    {
        char buf[32];
        if (std::snprintf(buf, sizeof(buf), "%.2f", last_latency_ms) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
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
    return -ENOTSUP;
}

}  // namespace vstreamer
