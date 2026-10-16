#include "components/h264_encoder_esp32p4.hpp"

#include "core/key_util.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_type.hpp"
#include <bfc/shared_sized_buffer.hpp>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

/* esp_h264_alloc.h has no extern "C" guard of its own. */
extern "C"
{
#include "esp_h264_alloc.h"
}
#include "esp_h264_enc_single_hw.h"
#include "esp_timer.h"

namespace vstreamer
{
namespace
{

/* Hardware limits (esp_h264). */
constexpr int k_min_dim = 80;
constexpr int k_max_width = 1920;
constexpr int k_max_height = 2032;
/* Bitstream buffer DMA target: cache-line aligned. */
constexpr uint32_t k_out_align = 64;
constexpr size_t   k_out_q_max = 8;

size_t frame_bytes(int w, int h)
{
    return static_cast<size_t>(w) * static_cast<size_t>(h) * 3 / 2;
}

int parse_size(std::string_view s, int *w, int *h)
{
    const size_t x = s.find_first_of("xX");
    if (x == std::string_view::npos)
    {
        return -EINVAL;
    }
    int64_t ww = 0;
    int64_t hh = 0;
    if (key_parse_i64(s.substr(0, x), &ww) < 0 || key_parse_i64(s.substr(x + 1), &hh) < 0)
    {
        return -EINVAL;
    }
    /* O_UYY_E_VYY pairs lines and the chroma is 2x2: both even. */
    if (ww < k_min_dim || hh < k_min_dim || ww > k_max_width || hh > k_max_height || (ww % 2) ||
        (hh % 2))
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

int parse_int(std::string_view v, int64_t lo, int64_t hi, int64_t *out)
{
    int64_t n = 0;
    if (key_parse_i64(v, &n) < 0 || n < lo || n > hi)
    {
        return -EINVAL;
    }
    *out = n;
    return 0;
}

int set_int(std::string *value, int64_t n)
{
    char buf[32];
    if (key_format_i64(n, buf, sizeof(buf)) < 0)
    {
        return -EINVAL;
    }
    *value = buf;
    return 0;
}

const std::vector<port_desc> &enc_input_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc       p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_RAW;
        p.caps.push_back(caps);
        port_caps_entry data {};
        data.sdu_type = sdu_type_e::O_UYY_E_VYY;
        p.caps.push_back(data);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

const std::vector<port_desc> &enc_output_ports()
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

h264_encoder_esp32p4::h264_encoder_esp32p4() = default;

h264_encoder_esp32p4::~h264_encoder_esp32p4()
{
    close();
}

std::string h264_encoder_esp32p4::name() const
{
    return "h264_encoder_esp32p4";
}

int h264_encoder_esp32p4::codec_open_locked()
{
    esp_h264_enc_cfg_hw_t cfg {};
    cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;
    cfg.gop = static_cast<uint8_t>(gop);
    cfg.fps = static_cast<uint8_t>(fps);
    cfg.res.width = static_cast<uint16_t>(width);
    cfg.res.height = static_cast<uint16_t>(height);
    cfg.rc.bitrate = bitrate;
    cfg.rc.qp_min = static_cast<uint8_t>(qp_min);
    cfg.rc.qp_max = static_cast<uint8_t>(qp_max);

    esp_h264_enc_handle_t enc = nullptr;
    if (esp_h264_enc_hw_new(&cfg, &enc) != ESP_H264_ERR_OK || nullptr == enc)
    {
        std::fprintf(stderr, "h264_encoder_esp32p4: esp_h264_enc_hw_new %dx%d failed\n", width,
                     height);
        return -EIO;
    }
    esp_h264_enc_param_hw_handle_t param = nullptr;
    if (esp_h264_enc_hw_get_param_hd(enc, &param) != ESP_H264_ERR_OK ||
        esp_h264_enc_open(enc) != ESP_H264_ERR_OK)
    {
        esp_h264_enc_del(enc);
        return -EIO;
    }

    /* Twice the raw frame: room for an IDR even at qp 0. A larger AU is not written past the
     * buffer; esp_h264 fails the frame (-EIO). */
    uint32_t actual = 0;
    auto    *out = static_cast<uint8_t *>(esp_h264_aligned_calloc(
        k_out_align, 1, static_cast<uint32_t>(2 * frame_bytes(width, height)), &actual,
        ESP_H264_MEM_SPIRAM));
    if (nullptr == out)
    {
        esp_h264_enc_close(enc);
        esp_h264_enc_del(enc);
        return -ENOMEM;
    }

    enc_ = enc;
    param_ = param;
    out_buf_ = out;
    out_cap_ = actual;
    live_w_ = width;
    live_h_ = height;
    reopen_req = false;
    pending_idr = false;
    return 0;
}

void h264_encoder_esp32p4::codec_close_locked()
{
    if (nullptr != enc_)
    {
        auto *enc = static_cast<esp_h264_enc_handle_t>(enc_);
        esp_h264_enc_close(enc);
        esp_h264_enc_del(enc);
        enc_ = nullptr;
        param_ = nullptr;
    }
    if (nullptr != out_buf_)
    {
        esp_h264_free(out_buf_);
        out_buf_ = nullptr;
        out_cap_ = 0;
    }
    live_w_ = 0;
    live_h_ = 0;
}

int h264_encoder_esp32p4::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    const int r = codec_open_locked();
    if (r < 0)
    {
        return r;
    }
    opened = true;
    return 0;
}

void h264_encoder_esp32p4::close()
{
    std::lock_guard<std::mutex> lock(mu);
    codec_close_locked();
    out_q_.clear();
    have_input_caps_ = false;
    caps_reject_ = false;
    caps_out_ = video_coded_caps {};
    opened = false;
    reopen_req = false;
}

int h264_encoder_esp32p4::input_caps_locked(const component_pdu &in)
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
    /* Sent ahead of the first AU of each size (and fps), including after a runtime resize. */
    if (caps.width != caps_out_.width || caps.height != caps_out_.height || fps != caps_out_.fps_num)
    {
        video_coded_caps coded {};
        coded.width = caps.width;
        coded.height = caps.height;
        coded.fps_num = fps;
        coded.fps_den = 1;
        out_q_.push_back(make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, coded, in.ts_us, 0));
        caps_out_ = coded;
        notify_wakeup();
    }
    return 0;
}

int h264_encoder_esp32p4::encode_locked(const component_pdu &in)
{
    if (reopen_req)
    {
        codec_close_locked();
        const int r = codec_open_locked();
        if (r < 0)
        {
            return r;
        }
    }
    if (nullptr == enc_)
    {
        return -EBADF;
    }
    if (in.sdu.size() != frame_bytes(live_w_, live_h_) || nullptr == in.sdu.u8())
    {
        return -EINVAL;
    }
    /* Back-pressure instead of dropping: the queue can hold the caps PDU or an IDR. */
    if (out_q_.size() >= k_out_q_max)
    {
        return -EAGAIN;
    }

    auto *enc = static_cast<esp_h264_enc_handle_t>(enc_);
    if (pending_idr)
    {
        (void)esp_h264_enc_force_idr(static_cast<esp_h264_enc_param_handle_t>(param_));
        pending_idr = false;
    }

    esp_h264_enc_in_frame_t in_frame {};
    /* The encoder only reads the frame (cache write-back, then DMA). */
    in_frame.raw_data.buffer = const_cast<uint8_t *>(in.sdu.u8());
    in_frame.raw_data.len = static_cast<uint32_t>(in.sdu.size());
    in_frame.pts = static_cast<uint32_t>(in.ts_us / 1000);
    esp_h264_enc_out_frame_t out_frame {};
    out_frame.raw_data.buffer = out_buf_;
    out_frame.raw_data.len = out_cap_;

    const int64_t t0 = esp_timer_get_time();
    const esp_h264_err_t err = esp_h264_enc_process(enc, &in_frame, &out_frame);
    const int64_t dt = esp_timer_get_time() - t0;
    if (err != ESP_H264_ERR_OK || 0 == out_frame.length || out_frame.length > out_cap_)
    {
        ++encode_errors_;
        return -EIO;
    }
    last_encode_us_ = dt;
    max_encode_us_ = dt > max_encode_us_ ? dt : max_encode_us_;

    component_pdu au;
    au.ts_us = in.ts_us;
    au.sdu_type = sdu_type_e::H264_AU;
    au.port = 0;
    const bool idr = out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR;
    au.flags = idr ? static_cast<uint8_t>(pdu_flag_e::KEY) : 0;
    au.sdu = bfc::shared_sized_buffer::copy_from(out_buf_, out_frame.length);
    if (au.sdu.empty())
    {
        return -ENOMEM;
    }
    out_q_.push_back(std::move(au));
    ++frames_;
    if (idr)
    {
        ++idr_frames_;
    }
    notify_wakeup();
    return 0;
}

int h264_encoder_esp32p4::input(component_pdu &&in)
{
    if (0 != in.port)
    {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }
    if (in.sdu_type == sdu_type_e::CAPS_VIDEO_RAW)
    {
        return input_caps_locked(in);
    }
    if (in.sdu_type != sdu_type_e::O_UYY_E_VYY)
    {
        return -EINVAL;
    }
    if (caps_reject_ || !have_input_caps_)
    {
        return -ENOTSUP;
    }
    return encode_locked(in);
}

int h264_encoder_esp32p4::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu);
    if (out_q_.empty())
    {
        return opened ? -EAGAIN : -EBADF;
    }
    out = std::move(out_q_.front());
    out_q_.pop_front();
    out.seq = out_seq_++;
    return 0;
}

int h264_encoder_esp32p4::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(mu);
    auto *param = static_cast<esp_h264_enc_param_handle_t>(param_);
    int64_t n = 0;

    if (key == "size")
    {
        int w = 0;
        int h = 0;
        const int r = parse_size(value, &w, &h);
        if (r < 0)
        {
            return r;
        }
        if (w != width || h != height)
        {
            width = w;
            height = h;
            reopen_req = opened;
            /* Raw caps of the old size no longer match. */
            have_input_caps_ = false;
        }
        return 0;
    }
    if (key == "fps")
    {
        if (parse_int(value, 1, 120, &n) < 0)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(n);
        if (nullptr != param &&
            esp_h264_enc_set_fps(param, static_cast<uint8_t>(fps)) != ESP_H264_ERR_OK)
        {
            return -EIO;
        }
        return 0;
    }
    if (key == "gop")
    {
        if (parse_int(value, 1, 255, &n) < 0)
        {
            return -EINVAL;
        }
        gop = static_cast<int>(n);
        if (nullptr != param &&
            esp_h264_enc_set_gop(param, static_cast<uint8_t>(gop)) != ESP_H264_ERR_OK)
        {
            return -EIO;
        }
        return 0;
    }
    if (key == "bitrate" || key == "bps")
    {
        if (parse_int(value, 10000, 100000000, &n) < 0)
        {
            return -EINVAL;
        }
        bitrate = static_cast<uint32_t>(n);
        if (nullptr != param && esp_h264_enc_set_bitrate(param, bitrate) != ESP_H264_ERR_OK)
        {
            return -EIO;
        }
        return 0;
    }
    if (key == "qp_min" || key == "qp_max")
    {
        if (parse_int(value, 0, 51, &n) < 0)
        {
            return -EINVAL;
        }
        const int lo = key == "qp_min" ? static_cast<int>(n) : qp_min;
        const int hi = key == "qp_max" ? static_cast<int>(n) : qp_max;
        if (lo > hi)
        {
            return -EINVAL;
        }
        if (lo != qp_min || hi != qp_max)
        {
            qp_min = lo;
            qp_max = hi;
            /* esp_h264 takes the QP range only at creation. */
            reopen_req = opened;
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

int h264_encoder_esp32p4::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    int r = port_caps_query(enc_input_ports(), true, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }
    r = port_caps_query(enc_output_ports(), false, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (key == "status")
    {
        *value = opened ? "open" : "closed";
        return 0;
    }
    if (key == "backend" || key == "codec")
    {
        *value = "esp_h264_hw";
        return 0;
    }
    if (key == "size")
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dx%d", width, height);
        *value = buf;
        return 0;
    }
    if (key == "fps")
    {
        return set_int(value, fps);
    }
    if (key == "gop")
    {
        return set_int(value, gop);
    }
    if (key == "bitrate" || key == "bps")
    {
        return set_int(value, bitrate);
    }
    if (key == "qp_min")
    {
        return set_int(value, qp_min);
    }
    if (key == "qp_max")
    {
        return set_int(value, qp_max);
    }
    if (key == "encode_ms")
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.1f/%.1f", last_encode_us_ / 1000.0,
                      max_encode_us_ / 1000.0);
        *value = buf;
        return 0;
    }
    if (key == "stats")
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "frames=%" PRIu64 " idr=%" PRIu64 " errors=%" PRIu64
                      " encode_ms=%.1f max=%.1f",
                      frames_, idr_frames_, encode_errors_, last_encode_us_ / 1000.0,
                      max_encode_us_ / 1000.0);
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
