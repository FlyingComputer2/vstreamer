#include "components/h264_encoder_intel.hpp"

#include "core/component_pdu.hpp"
#include "core/key_util.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace vstreamer
{
namespace
{

constexpr size_t k_max_au = 2ULL * 1024ULL * 1024ULL;
constexpr size_t k_out_q_max = 32;

void nv12_keep(void * /*opaque*/, uint8_t * /*data*/)
{
}

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

int parse_low_power(std::string_view v, int *out)
{
    if (v == "auto")
    {
        *out = -1;
        return 0;
    }
    if (v == "0" || v == "1")
    {
        *out = v[0] - '0';
        return 0;
    }
    return -EINVAL;
}

bool h264_au_has_idr(const uint8_t *data, size_t len)
{
    if (nullptr == data || len < 5)
    {
        return false;
    }
    size_t i = 0;
    while (i + 4 < len)
    {
        if (data[i] == 0 && data[i + 1] == 0)
        {
            size_t off = 0;
            if (data[i + 2] == 1)
            {
                off = 3;
            }
            else if (data[i + 2] == 0 && data[i + 3] == 1)
            {
                off = 4;
            }
            if (off > 0 && i + off < len)
            {
                const int nal_type = data[i + off] & 0x1f;
                if (5 == nal_type)
                {
                    return true;
                }
                i += off + 1;
                continue;
            }
        }
        ++i;
    }
    return false;
}

void apply_rc_to_ctx(AVCodecContext *ctx, bool cbr, int bitrate, int qp_val, int gop_val, int vbv)
{
    ctx->max_b_frames = 0;
    if (cbr)
    {
        ctx->bit_rate = bitrate;
        ctx->rc_max_rate = bitrate;
        ctx->rc_buffer_size =
            static_cast<int>(static_cast<int64_t>(bitrate) * static_cast<int64_t>(vbv) / 1000);
        av_opt_set(ctx->priv_data, "rc_mode", "CBR", 0);
    }
    else
    {
        ctx->bit_rate = 0;
        ctx->rc_max_rate = 0;
        ctx->rc_buffer_size = 0;
        av_opt_set(ctx->priv_data, "rc_mode", "CQP", 0);
        av_opt_set_int(ctx->priv_data, "qp", qp_val, 0);
    }
    // idr_interval counts I-frames between IDRs, not frames. Keep it at 0 so every I-frame
    // is an IDR with in-band SPS/PPS; gop_size sets the spacing.
    (void)gop_val;
    av_opt_set_int(ctx->priv_data, "idr_interval", 0, 0);
}

}  // namespace

h264_encoder_intel::h264_encoder_intel() = default;

h264_encoder_intel::~h264_encoder_intel()
{
    close();
}

std::string h264_encoder_intel::name() const
{
    return "h264_encoder_intel";
}



int h264_encoder_intel::nv12_size_locked() const
{
    int w = live_w > 0 ? live_w : width;
    int h = live_h > 0 ? live_h : height;
    return av_image_get_buffer_size(AV_PIX_FMT_NV12, w, h, 1);
}

void h264_encoder_intel::clear_out_locked()
{
    out_q.clear();
    pending_caps_out_.clear();
    have_input_caps_ = false;
    caps_reject_ = false;
    have_output_caps_ = false;
}

void h264_encoder_intel::log_opened_locked() const
{
    const char *rc = live_rc_cbr ? "cbr" : "cqp";
    std::fprintf(stderr,
                 "h264_encoder_intel: opened %dx%d@%d rc=%s bps=%d vbv_ms=%d gop=%d low_power=%d\n",
                 live_w, live_h, live_fps, rc, live_bps, live_vbv_ms, live_gop, low_power_live);
}

int h264_encoder_intel::drain_packets_locked()
{
    auto *ctx = static_cast<AVCodecContext *>(this->ctx);
    auto *pkt = static_cast<AVPacket *>(this->pkt);
    if (nullptr == ctx || nullptr == pkt)
    {
        return -EBADF;
    }

    for (;;)
    {
        int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return 0;
        }
        if (ret < 0)
        {
            return ret;
        }

        if (pkt->size <= 0 || static_cast<size_t>(pkt->size) > k_max_au)
        {
            av_packet_unref(pkt);
            continue;
        }

        while (out_q.size() >= k_out_q_max)
        {
            out_q.pop_front();
        }

        auto *buf = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(pkt->size)));
        if (nullptr == buf)
        {
            av_packet_unref(pkt);
            return -ENOMEM;
        }
        const size_t sz = static_cast<size_t>(pkt->size);
        std::memcpy(buf, pkt->data, sz);

        shared_sized_buffer payload = shared_sized_buffer::adopt(
            reinterpret_cast<std::byte *>(buf), sz, sz, [](std::byte *p) {
                std::free(reinterpret_cast<uint8_t *>(p));
            });
        int64_t out_pts = pkt->pts;
        if (out_pts == AV_NOPTS_VALUE)
        {
            out_pts = last_out_pts >= 0 ? last_out_pts + 1 : 0;
        }
        if (last_out_pts >= 0 && out_pts < last_out_pts)
        {
            out_pts = last_out_pts;
        }
        last_out_pts = out_pts;
        const bool key = h264_au_has_idr(buf, sz);
        component_pdu au;
        au.ts_us = static_cast<uint64_t>(out_pts);
        au.seq = 0;
        au.sdu_type = sdu_type_e::H264_AU;
        au.port = 0;
        au.flags = key ? static_cast<uint8_t>(pdu_flag_e::KEY) : 0;
        au.sdu = std::move(payload);
        out_q.push_back(std::move(au));
        av_packet_unref(pkt);
        notify_wakeup();
        cv.notify_one();
    }
}

int h264_encoder_intel::codec_open_locked()
{
    const char *dev = device.empty() ? nullptr : device.c_str();

    AVBufferRef *hw_dev = nullptr;
    int ret = av_hwdevice_ctx_create(&hw_dev, AV_HWDEVICE_TYPE_VAAPI, dev, nullptr, 0);
    if (ret < 0)
    {
        std::fprintf(stderr, "h264_encoder_intel: VAAPI device open failed");
        if (!device.empty())
        {
            std::fprintf(stderr, " (%s)", device.c_str());
        }
        std::fprintf(stderr, "\n");
        return -EIO;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (nullptr == codec)
    {
        std::fprintf(stderr, "h264_encoder_intel: h264_vaapi not found\n");
        av_buffer_unref(&hw_dev);
        return -ENOENT;
    }

    AVBufferRef *hw_frames = av_hwframe_ctx_alloc(hw_dev);
    if (nullptr == hw_frames)
    {
        av_buffer_unref(&hw_dev);
        return -ENOMEM;
    }

    AVHWFramesContext *frames = reinterpret_cast<AVHWFramesContext *>(hw_frames->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = width;
    frames->height = height;
    frames->initial_pool_size = 16;
    ret = av_hwframe_ctx_init(hw_frames);
    if (ret < 0)
    {
        av_buffer_unref(&hw_frames);
        av_buffer_unref(&hw_dev);
        return -EIO;
    }

    const bool try_auto_lp = (low_power_cfg < 0);
    int lp_try[2] = {low_power_cfg >= 0 ? low_power_cfg : 0, 1};
    int lp_count = try_auto_lp ? 2 : 1;

    AVCodecContext *ctx = nullptr;
    AVFrame *sw = nullptr;
    AVFrame *hw = nullptr;
    AVPacket *pkt = nullptr;
    int resolved_lp = -1;

    for (int attempt = 0; attempt < lp_count; ++attempt)
    {
        const int lp = lp_try[attempt];

        if (ctx)
        {
            avcodec_free_context(&ctx);
        }
        ctx = avcodec_alloc_context3(codec);
        sw = av_frame_alloc();
        hw = av_frame_alloc();
        pkt = av_packet_alloc();
        if (nullptr == ctx || nullptr == sw || nullptr == hw || nullptr == pkt)
        {
            av_frame_free(&sw);
            av_frame_free(&hw);
            av_packet_free(&pkt);
            avcodec_free_context(&ctx);
            av_buffer_unref(&hw_frames);
            av_buffer_unref(&hw_dev);
            return -ENOMEM;
        }

        ctx->width = width;
        ctx->height = height;
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;
        ctx->time_base = AVRational{1, fps};
        ctx->framerate = AVRational{fps, 1};
        ctx->gop_size = gop > 0 ? gop : 1;

        ctx->hw_frames_ctx = av_buffer_ref(hw_frames);
        ctx->hw_device_ctx = av_buffer_ref(hw_dev);
        if (nullptr == ctx->hw_frames_ctx || nullptr == ctx->hw_device_ctx)
        {
            av_frame_free(&sw);
            av_frame_free(&hw);
            av_packet_free(&pkt);
            avcodec_free_context(&ctx);
            av_buffer_unref(&hw_frames);
            av_buffer_unref(&hw_dev);
            return -ENOMEM;
        }

        apply_rc_to_ctx(ctx, rc_cbr, bps, qp, gop, vbv_ms);
        av_opt_set_int(ctx->priv_data, "low_power", lp, 0);

        if (avcodec_open2(ctx, codec, nullptr) >= 0)
        {
            resolved_lp = lp;
            if (try_auto_lp && attempt == 1)
            {
                std::fprintf(stderr, "h264_encoder_intel: opened with low_power=1 after retry\n");
            }
            break;
        }

        av_frame_free(&sw);
        av_frame_free(&hw);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        ctx = nullptr;
        sw = nullptr;
        hw = nullptr;
        pkt = nullptr;

        if (!try_auto_lp || attempt + 1 >= lp_count)
        {
            std::fprintf(stderr, "h264_encoder_intel: open h264_vaapi failed\n");
            av_buffer_unref(&hw_frames);
            av_buffer_unref(&hw_dev);
            return -EIO;
        }
    }

    av_buffer_unref(&hw_frames);

    this->hw_device = hw_dev;
    this->ctx = ctx;
    this->swframe = sw;
    this->hwframe = hw;
    this->pkt = pkt;
    live_w = width;
    live_h = height;
    live_fps = fps;
    live_qp = qp;
    live_gop = gop;
    live_bps = bps;
    live_rc_cbr = rc_cbr;
    live_vbv_ms = vbv_ms;
    live_low_power_cfg = low_power_cfg;
    low_power_live = resolved_lp;
    reopen_req = false;
    last_out_pts = -1;
    pending_idr = true;
    return 0;
}

void h264_encoder_intel::codec_close_locked()
{
    if (ctx)
    {
        auto *c = static_cast<AVCodecContext *>(ctx);
        avcodec_send_frame(c, nullptr);
        (void)drain_packets_locked();
    }

    if (swframe)
    {
        AVFrame *f = static_cast<AVFrame *>(swframe);
        av_frame_free(&f);
        swframe = nullptr;
    }
    if (hwframe)
    {
        AVFrame *f = static_cast<AVFrame *>(hwframe);
        av_frame_free(&f);
        hwframe = nullptr;
    }
    if (pkt)
    {
        AVPacket *p = static_cast<AVPacket *>(pkt);
        av_packet_free(&p);
        pkt = nullptr;
    }
    if (ctx)
    {
        AVCodecContext *c = static_cast<AVCodecContext *>(ctx);
        avcodec_free_context(&c);
        ctx = nullptr;
    }
    if (hw_device)
    {
        AVBufferRef *d = static_cast<AVBufferRef *>(hw_device);
        av_buffer_unref(&d);
        hw_device = nullptr;
    }
    live_w = 0;
    live_h = 0;
    live_fps = 0;
    live_qp = 0;
    live_gop = 0;
    live_bps = 0;
    live_rc_cbr = false;
    live_vbv_ms = 0;
    live_low_power_cfg = -1;
    low_power_live = -1;
}

int h264_encoder_intel::reopen_if_needed_locked()
{
    if (!reopen_req && ctx)
    {
        return 0;
    }
    codec_close_locked();
    int r = codec_open_locked();
    if (r < 0)
    {
        return r;
    }
    log_opened_locked();
    return 0;
}

int h264_encoder_intel::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    int r = codec_open_locked();
    if (r < 0)
    {
        return r;
    }
    opened = true;
    log_opened_locked();
    return 0;
}

void h264_encoder_intel::close()
{
    std::lock_guard<std::mutex> lock(mu);
    codec_close_locked();
    clear_out_locked();
    opened = false;
    reopen_req = false;
    cv.notify_all();
}



int h264_encoder_intel::configure(std::string_view key, std::string_view value)
{
    std::string_view v = value;
    std::string tmp(v);

    std::lock_guard<std::mutex> lock(mu);

    if (key == "device")
    {
        std::string nv(v);
        if (nv != device)
        {
            device = std::move(nv);
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "size")
    {
        int w = 0;
        int h = 0;
        int r = parse_size(v, &w, &h);
        if (r < 0)
        {
            return r;
        }
        if (w != width || h != height)
        {
            width = w;
            height = h;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "fps")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 1 || n > 120)
        {
            return -EINVAL;
        }
        int nv = static_cast<int>(n);
        if (nv != fps)
        {
            fps = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "qp")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 0 || n > 52)
        {
            return -EINVAL;
        }
        int nv = static_cast<int>(n);
        if (nv != qp)
        {
            qp = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "gop")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 1 || n > 255)
        {
            return -EINVAL;
        }
        int nv = static_cast<int>(n);
        if (nv != gop)
        {
            gop = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "cbr" || key == "bps")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 0 || n > 200000000LL)
        {
            return -EINVAL;
        }
        const int nv = static_cast<int>(n);
        if (nv != bps)
        {
            bps = nv;
            if (opened && rc_cbr)
            {
                reopen_req = true;
                pending_idr = true;
            }
        }
        return 0;
    }
    if (key == "rc")
    {
        bool want_cbr = (tmp == "cbr");
        bool want_cqp = (tmp == "cqp" || tmp == "fixqp");
        if (!want_cbr && !want_cqp)
        {
            return -EINVAL;
        }
        const bool nv = want_cbr;
        if (nv != rc_cbr)
        {
            rc_cbr = nv;
            if (opened)
            {
                reopen_req = true;
                pending_idr = true;
            }
        }
        return 0;
    }
    if (key == "low_power")
    {
        int nv = 0;
        if (parse_low_power(v, &nv) < 0)
        {
            return -EINVAL;
        }
        if (nv != low_power_cfg)
        {
            low_power_cfg = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "vbv_ms")
    {
        int64_t n = 0;
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 1 || n > 2000)
        {
            return -EINVAL;
        }
        const int nv = static_cast<int>(n);
        if (nv != vbv_ms)
        {
            vbv_ms = nv;
            if (opened)
            {
                reopen_req = true;
            }
        }
        return 0;
    }
    if (key == "idr")
    {
        pending_idr = true;
        return 0;
    }
    return -ENOTSUP;
}

namespace
{

const std::vector<port_desc> &intel_enc_input_ports()
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

const std::vector<port_desc> &intel_enc_output_ports()
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

}  // namespace

int h264_encoder_intel::input(component_pdu &&in)
{
    if (in.sdu_type == sdu_type_e::CAPS_VIDEO_RAW)
    {
        video_raw_caps caps {};
        if (read_caps(in, &caps) != 0)
        {
            return -EINVAL;
        }
        if (caps.width != width || caps.height != height)
        {
            caps_reject_ = true;
            have_input_caps_ = false;
            return -ENOTSUP;
        }
        caps_reject_ = false;
        have_input_caps_ = true;
        input_caps_ = caps;
        if (!have_output_caps_ || caps.width != output_caps_.width || caps.height != output_caps_.height)
        {
            video_coded_caps coded {};
            coded.width = caps.width;
            coded.height = caps.height;
            coded.fps_num = fps;
            coded.fps_den = 1;
            output_caps_ = coded;
            have_output_caps_ = true;
            component_pdu caps_pdu =
                make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, coded, in.ts_us, 0);
            caps_pdu.seq = 0;
            pending_caps_out_.push_back(std::move(caps_pdu));
        }
        return 0;
    }
    if (in.sdu_type != sdu_type_e::NV12)
    {
        return -EINVAL;
    }
    if (caps_reject_ || !have_input_caps_)
    {
        return -ENOTSUP;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }

    int r = reopen_if_needed_locked();
    if (r < 0)
    {
        return r;
    }

    if (input_caps_.width != live_w || input_caps_.height != live_h)
    {
        return -EINVAL;
    }
    int want = nv12_size_locked();
    if (want < 0 || static_cast<size_t>(want) != in.sdu.size() || nullptr == in.sdu.u8())
    {
        return -EINVAL;
    }

    auto *ctx = static_cast<AVCodecContext *>(this->ctx);
    auto *sw = static_cast<AVFrame *>(this->swframe);
    auto *hw = static_cast<AVFrame *>(this->hwframe);

    av_frame_unref(sw);
    int sz = av_image_fill_arrays(sw->data, sw->linesize, in.sdu.u8(), AV_PIX_FMT_NV12, live_w,
                                  live_h, 1);
    if (sz < 0)
    {
        return sz;
    }
    sw->width = live_w;
    sw->height = live_h;
    sw->format = AV_PIX_FMT_NV12;
    sw->pts = static_cast<int64_t>(in.ts_us);
    const bool mark_key_au = pending_idr;
    pending_idr = false;
    sw->buf[0] = av_buffer_create(in.sdu.u8(), static_cast<size_t>(sz), nv12_keep, nullptr, 0);
    if (nullptr == sw->buf[0])
    {
        av_frame_unref(sw);
        return AVERROR(ENOMEM);
    }

    av_frame_unref(hw);
    hw->format = AV_PIX_FMT_VAAPI;
    int ret = av_hwframe_get_buffer(ctx->hw_frames_ctx, hw, 0);
    if (ret < 0)
    {
        av_frame_unref(sw);
        return ret;
    }
    ret = av_hwframe_transfer_data(hw, sw, 0);
    av_frame_unref(sw);
    if (ret < 0)
    {
        av_frame_unref(hw);
        return ret;
    }
    hw->pts = static_cast<int64_t>(in.ts_us);
    if (mark_key_au)
    {
        hw->pict_type = AV_PICTURE_TYPE_I;
#ifdef AV_FRAME_FLAG_KEY
        hw->flags |= AV_FRAME_FLAG_KEY;
#endif
    }

    ret = avcodec_send_frame(ctx, hw);
    av_frame_unref(hw);
    if (ret < 0)
    {
        return ret;
    }
    return drain_packets_locked();
}

int h264_encoder_intel::output(component_pdu &out)
{
    if (!pending_caps_out_.empty())
    {
        out = std::move(pending_caps_out_.front());
        pending_caps_out_.pop_front();
        return 0;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!opened && out_q.empty())
    {
        return -EBADF;
    }
    if (out_q.empty())
    {
        return -EAGAIN;
    }

    out = std::move(out_q.front());
    out_q.pop_front();
    if (out.seq == 0)
    {
        out.seq = out_seq_++;
    }
    notify_wakeup();
    return 0;
}

int h264_encoder_intel::query(std::string_view key, std::string *value) const
{
    int r = port_caps_query(intel_enc_input_ports(), true, key, value);
    if (0 == r)
    {
        return 0;
    }
    r = port_caps_query(intel_enc_output_ports(), false, key, value);
    if (0 == r)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mu);

    if (key == "status")
    {
        *value = opened ? "open" : "closed";
        return 0;
    }
    if (key == "device")
    {
        *value = device;
        return 0;
    }
    if (key == "size")
    {
        char buf[64];
        int  w = live_w > 0 ? live_w : width;
        int  h = live_h > 0 ? live_h : height;
        if (std::snprintf(buf, sizeof(buf), "%dx%d", w, h) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "fps" || key == "qp" || key == "gop")
    {
        int n = 0;
        if (key == "fps")
        {
            n = live_fps > 0 ? live_fps : fps;
        }
        else if (key == "qp")
        {
            n = live_qp > 0 ? live_qp : qp;
        }
        else
        {
            n = live_gop > 0 ? live_gop : gop;
        }
        char buf[32];
        if (key_format_i64(n, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "rc")
    {
        const bool cbr = opened ? live_rc_cbr : rc_cbr;
        *value = cbr ? "cbr" : "cqp";
        return 0;
    }
    if (key == "cbr" || key == "bps")
    {
        const int n = opened && live_bps > 0 ? live_bps : bps;
        char buf[32];
        if (key_format_i64(n, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "low_power")
    {
        if (opened && low_power_live >= 0)
        {
            char buf[8];
            if (std::snprintf(buf, sizeof(buf), "%d", low_power_live) < 0)
            {
                return -EINVAL;
            }
            *value = buf;
            return 0;
        }
        if (low_power_cfg < 0)
        {
            *value = "auto";
            return 0;
        }
        char buf[8];
        if (std::snprintf(buf, sizeof(buf), "%d", low_power_cfg) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "vbv_ms")
    {
        const int n = opened && live_vbv_ms > 0 ? live_vbv_ms : vbv_ms;
        char buf[32];
        if (key_format_i64(n, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "backend" || key == "codec")
    {
        *value = "h264_vaapi";
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
