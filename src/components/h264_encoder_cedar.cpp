#include "components/h264_encoder_cedar.hpp"

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
    /* Cedar VE stride: width multiple of 32; both even. */
    if (ww < 32 || hh < 2 || (ww % 32) || (hh % 2) || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

}  // namespace

h264_encoder_cedar::h264_encoder_cedar() = default;

h264_encoder_cedar::~h264_encoder_cedar()
{
    close();
}

std::string h264_encoder_cedar::name() const
{
    return "h264_encoder_cedar";
}



int h264_encoder_cedar::nv12_size_locked() const
{
    int w = live_w > 0 ? live_w : width;
    int h = live_h > 0 ? live_h : height;
    return av_image_get_buffer_size(AV_PIX_FMT_NV12, w, h, 1);
}

void h264_encoder_cedar::clear_out_locked()
{
    out_q.clear();
    in_capture_ts_us_.clear();
    pending_caps_out_.clear();
    have_input_caps_ = false;
    caps_reject_ = false;
    have_output_caps_ = false;
}

int h264_encoder_cedar::drain_packets_locked()
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
        uint64_t capture_ts = 0;
        if (!in_capture_ts_us_.empty())
        {
            capture_ts = in_capture_ts_us_.front();
            in_capture_ts_us_.pop_front();
        }
        else if (pkt->pts != AV_NOPTS_VALUE)
        {
            capture_ts = static_cast<uint64_t>(pkt->pts);
        }
        component_pdu au;
        au.ts_us = capture_ts;
        au.seq = 0;
        au.sdu_type = sdu_type_e::H264_AU;
        au.port = 0;
        au.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? static_cast<uint8_t>(pdu_flag_e::KEY) : 0;
        au.sdu = std::move(payload);
        out_q.push_back(std::move(au));
        av_packet_unref(pkt);
        notify_wakeup();
        cv.notify_one();
    }
}

int h264_encoder_cedar::codec_open_locked()
{
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_cedrus");
    if (nullptr == codec)
    {
        std::fprintf(stderr, "h264_encoder_cedar: h264_cedrus not found\n");
        return -ENOENT;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (nullptr == ctx || nullptr == frame || nullptr == pkt)
    {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        return -ENOMEM;
    }

    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->time_base = AVRational{1, 1000000};
    ctx->framerate = AVRational{fps, 1};
    ctx->gop_size = gop > 0 ? gop : 1;
    av_opt_set_int(ctx->priv_data, "qp", qp, 0);

    if (avcodec_open2(ctx, codec, nullptr) < 0)
    {
        std::fprintf(stderr,
                     "h264_encoder_cedar: open h264_cedrus failed (is /dev/cedar_dev free?)\n");
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        return -EIO;
    }

    this->ctx = ctx;
    this->avframe = frame;
    this->pkt = pkt;
    live_w = width;
    live_h = height;
    live_fps = fps;
    live_qp = qp;
    live_gop = gop;
    reopen_req = false;
    return 0;
}

void h264_encoder_cedar::codec_close_locked()
{
    if (ctx)
    {
        auto *c = static_cast<AVCodecContext *>(ctx);
        avcodec_send_frame(c, nullptr);
        (void)drain_packets_locked();
    }

    if (avframe)
    {
        AVFrame *f = static_cast<AVFrame *>(avframe);
        av_frame_free(&f);
        avframe = nullptr;
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
    live_w = 0;
    live_h = 0;
    live_fps = 0;
    live_qp = 0;
    live_gop = 0;
}

int h264_encoder_cedar::reopen_if_needed_locked()
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
    std::fprintf(stderr, "h264_encoder_cedar: opened %dx%d@%d qp=%d gop=%d\n", live_w, live_h,
                 live_fps, live_qp, live_gop);
    return 0;
}

int h264_encoder_cedar::open()
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
    std::fprintf(stderr, "h264_encoder_cedar: opened %dx%d@%d qp=%d gop=%d\n", live_w, live_h,
                 live_fps, live_qp, live_gop);
    return 0;
}

void h264_encoder_cedar::close()
{
    std::lock_guard<std::mutex> lock(mu);
    codec_close_locked();
    clear_out_locked();
    opened = false;
    reopen_req = false;
    cv.notify_all();
}



int h264_encoder_cedar::configure(std::string_view key, std::string_view value)
{
    std::string_view v = value;
    std::string tmp(v);

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
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 2 || n > 47)
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
    if (key == "idr")
    {
        pending_idr = true;
        return 0;
    }
    return -ENOTSUP;
}

namespace
{

const std::vector<port_desc> &cedar_enc_input_ports()
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

const std::vector<port_desc> &cedar_enc_output_ports()
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

int h264_encoder_cedar::input(component_pdu &&in)
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
    auto *frame = static_cast<AVFrame *>(this->avframe);

    av_frame_unref(frame);
    int sz = av_image_fill_arrays(frame->data, frame->linesize, in.sdu.u8(), AV_PIX_FMT_NV12,
                                  live_w, live_h, 1);
    if (sz < 0)
    {
        return sz;
    }
    frame->width = live_w;
    frame->height = live_h;
    frame->format = AV_PIX_FMT_NV12;
    frame->pts = static_cast<int64_t>(in.ts_us);
    if (pending_idr)
    {
        frame->pict_type = AV_PICTURE_TYPE_I;
#ifdef AV_FRAME_FLAG_KEY
        frame->flags |= AV_FRAME_FLAG_KEY;
#endif
        pending_idr = false;
    }
    frame->buf[0] =
        av_buffer_create(in.sdu.u8(), static_cast<size_t>(sz), nv12_keep, nullptr, 0);
    if (nullptr == frame->buf[0])
    {
        av_frame_unref(frame);
        return AVERROR(ENOMEM);
    }

    const uint64_t capture_ts_us = in.ts_us;
    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0)
    {
        av_frame_unref(frame);
        return ret;
    }
    in_capture_ts_us_.push_back(capture_ts_us);

    r = drain_packets_locked();
    av_frame_unref(frame);
    return r;
}

int h264_encoder_cedar::output(component_pdu &out)
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

int h264_encoder_cedar::query(std::string_view key, std::string *value) const
{
    int r = port_caps_query(cedar_enc_input_ports(), true, key, value);
    if (0 == r)
    {
        return 0;
    }
    r = port_caps_query(cedar_enc_output_ports(), false, key, value);
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
    if (key == "backend" || key == "codec")
    {
        *value = "h264_cedrus";
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
