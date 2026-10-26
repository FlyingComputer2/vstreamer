#include "components/h264_encoder_esp32p4.hpp"

#include "core/key_util.hpp"
#include "core/latency_query.hpp"
#include "core/time_util.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_type.hpp"
#include <bfc/shared_sized_buffer.hpp>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
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
/* A 1080p frame at qp 10 overflowed 768 KiB; a larger frame is dropped and the next is an IDR. */
constexpr uint32_t k_max_bitstream_bytes = 1024U * 1024U;
/* AUs downstream (pay, sender) hold slots; a slot short means an encode waits. */
constexpr size_t   k_out_slot_count = 6;
/* Buffered bits are repaid over this long; the buffer is clamped to [-0.5, +1] s of bitrate. */
constexpr double k_rc_repay_s = 0.5;
/* A P frame's size also depends on its reference's QP, so a full per-frame correction rings at
 * a 4-frame period; filter the size and move a quarter of the way, one QP per frame at most. */
constexpr double k_rc_ema = 0.25;
constexpr double k_rc_gain = 0.25;
constexpr int    k_rc_max_step = 1;

/* hw_dirty_ bits. */
constexpr unsigned k_hw_bitrate = 1U << 0;
constexpr unsigned k_hw_gop = 1U << 1;
constexpr unsigned k_hw_fps = 1U << 2;

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
    /* Frame RC: a fixed slice QP of qp_min, raised per frame through the non-ROI delta. A
     * negative delta hangs the encoder (1 s timeout on every frame), so the base is the floor. */
    cfg.rc.qp_min = static_cast<uint8_t>(qp_min);
    cfg.rc.qp_max = static_cast<uint8_t>(frame_rc ? qp_min : qp_max);

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

    if (frame_rc)
    {
        esp_h264_enc_roi_cfg_t roi {};
        roi.roi_mode = ESP_H264_ROI_MODE_DELTA_QP;
        roi.none_roi_delta_qp = 0;
        if (esp_h264_enc_hw_cfg_roi(param, roi) != ESP_H264_ERR_OK)
        {
            esp_h264_enc_close(enc);
            esp_h264_enc_del(enc);
            return -EIO;
        }
        rc_qp_ = std::clamp((qp_min + qp_max) / 2, qp_min, qp_max);
        rc_buf_bits_ = 0;
        rc_p_bits_ = 0;
    }

    /* PSRAM: internal RAM is kept for DMA, stacks and the network stack. */
    const uint32_t want_out =
        std::min(static_cast<uint32_t>(2 * frame_bytes(width, height)), k_max_bitstream_bytes);
    auto pool = std::make_shared<slot_pool>();
    pool->slots.resize(k_out_slot_count);
    for (auto &slot : pool->slots)
    {
        uint32_t actual = 0;
        slot.buf = static_cast<uint8_t *>(
            esp_h264_aligned_calloc(k_out_align, 1, want_out, &actual, ESP_H264_MEM_SPIRAM));
        if (nullptr == slot.buf)
        {
            pool->retire();
            esp_h264_enc_close(enc);
            esp_h264_enc_del(enc);
            return -ENOMEM;
        }
        slot.cap = actual;
    }
    pool->wake = [this] { notify_wakeup(); };
    pool_ = std::move(pool);

    enc_ = enc;
    param_ = param;
    live_w_ = width;
    live_h_ = height;
    reopen_req = false;
    pending_idr = false;
    hw_dirty_ = 0; /* the new codec was created with the current settings */
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
    if (pool_)
    {
        pool_->retire();
        pool_.reset();
    }
    live_w_ = 0;
    live_h_ = 0;
}

h264_encoder_esp32p4::slot_pool::~slot_pool()
{
    for (const slot &s : slots)
    {
        if (nullptr != s.buf)
        {
            esp_h264_free(s.buf);
        }
    }
}

void h264_encoder_esp32p4::slot_pool::release(size_t index)
{
    std::lock_guard<std::mutex> lock(mu);
    if (index >= slots.size())
    {
        return;
    }
    if (retired)
    {
        esp_h264_free(slots[index].buf);
        slots[index].buf = nullptr;
        return;
    }
    slots[index].free = true;
    /* Under mu: retire() clears wake under it too, so the encoder cannot be gone mid-call. */
    if (wake)
    {
        wake();
    }
}

void h264_encoder_esp32p4::slot_pool::retire()
{
    std::lock_guard<std::mutex> lock(mu);
    retired = true;
    wake = nullptr;
    for (slot &s : slots)
    {
        if (s.free && nullptr != s.buf)
        {
            esp_h264_free(s.buf);
            s.buf = nullptr;
        }
    }
}

int h264_encoder_esp32p4::acquire_out_slot_locked(size_t *index)
{
    if (!pool_)
    {
        return -EBADF;
    }
    std::lock_guard<std::mutex> lock(pool_->mu);
    for (size_t i = 0; i < pool_->slots.size(); ++i)
    {
        auto &slot = pool_->slots[i];
        if (slot.free && nullptr != slot.buf)
        {
            slot.free = false;
            *index = i;
            return 0;
        }
    }
    return -EAGAIN;
}

int h264_encoder_esp32p4::open()
{
    std::lock_guard<std::mutex> hw(hw_mu_);
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
    std::deque<component_pdu> queued;
    {
        std::lock_guard<std::mutex> hw(hw_mu_);
        std::lock_guard<std::mutex> lock(mu);
        codec_close_locked();
        queued.swap(out_q_);
        have_input_caps_ = false;
        caps_reject_ = false;
        caps_out_ = video_coded_caps {};
        opened = false;
        reopen_req = false;
    }
    /* Releasing an AU frees its slot through the retired pool; done outside mu all the same. */
    queued.clear();
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

/*
 * Frame-level rate control. The virtual buffer holds coded bits minus the budget; the next P
 * frame aims at the budget less 1/(fps * repay) of the buffer, and the QP moves a fraction of the
 * step that size needs (6 QP per halving), at most k_rc_max_step per frame. An IDR's size only
 * goes into the buffer: it is not a prediction of the next P frame.
 */
void h264_encoder_esp32p4::rc_update_locked(double bits, bool idr)
{
    const double budget = static_cast<double>(bitrate) / fps;
    const double second = static_cast<double>(bitrate);
    rc_buf_bits_ = std::clamp(rc_buf_bits_ + bits - budget, -0.5 * second, second);
    if (idr || bits <= 0)
    {
        return;
    }
    rc_p_bits_ = rc_p_bits_ > 0 ? rc_p_bits_ + k_rc_ema * (bits - rc_p_bits_) : bits;
    const double target = std::max(budget - rc_buf_bits_ / (fps * k_rc_repay_s), 0.25 * budget);
    const double step = k_rc_gain * 6.0 * std::log2(rc_p_bits_ / target);
    const int    d = std::clamp(static_cast<int>(std::lround(step)), -k_rc_max_step, k_rc_max_step);
    rc_qp_ = std::clamp(rc_qp_ + d, qp_min, qp_max);
}

/* Called with hw_mu_ and lock (on mu) held; drops mu for the esp_h264 calls. */
int h264_encoder_esp32p4::encode_locked(const component_pdu &in, std::unique_lock<std::mutex> &lock)
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
    size_t out_index = 0;
    if (acquire_out_slot_locked(&out_index) < 0)
    {
        return -EAGAIN;
    }
    /* The slot is ours until released: buf and cap do not change while it is not free. */
    const std::shared_ptr<slot_pool> pool = pool_;
    uint8_t *const                   slot_buf = pool->slots[out_index].buf;
    const uint32_t                   slot_cap = pool->slots[out_index].cap;

    /* Everything the hardware calls need, taken under mu; enc_ and param_ stay valid without it
     * because only hw_mu_ holders open or close the codec. */
    auto *enc = static_cast<esp_h264_enc_handle_t>(enc_);
    void *const    param_raw = param_;
    auto *const    param = static_cast<esp_h264_enc_param_handle_t>(param_raw);
    const bool     force_idr = pending_idr;
    const unsigned dirty = hw_dirty_;
    const uint32_t bitrate_now = bitrate;
    const int      gop_now = gop;
    const int      fps_now = fps;
    const bool     frame_rc_now = frame_rc;
    const int      qp_used = rc_qp_;
    const int      delta_qp = qp_used - qp_min;
    pending_idr = false;
    hw_dirty_ = 0;
    lock.unlock();

    if (0 != (dirty & k_hw_bitrate) && esp_h264_enc_set_bitrate(param, bitrate_now) != ESP_H264_ERR_OK)
    {
        std::fprintf(stderr, "h264_encoder_esp32p4: set bitrate %" PRIu32 " failed\n", bitrate_now);
    }
    if (0 != (dirty & k_hw_gop) &&
        esp_h264_enc_set_gop(param, static_cast<uint8_t>(gop_now)) != ESP_H264_ERR_OK)
    {
        std::fprintf(stderr, "h264_encoder_esp32p4: set gop %d failed\n", gop_now);
    }
    if (0 != (dirty & k_hw_fps) &&
        esp_h264_enc_set_fps(param, static_cast<uint8_t>(fps_now)) != ESP_H264_ERR_OK)
    {
        std::fprintf(stderr, "h264_encoder_esp32p4: set fps %d failed\n", fps_now);
    }
    if (force_idr)
    {
        (void)esp_h264_enc_force_idr(param);
    }
    if (frame_rc_now)
    {
        esp_h264_enc_roi_cfg_t roi {};
        roi.roi_mode = ESP_H264_ROI_MODE_DELTA_QP;
        roi.none_roi_delta_qp = static_cast<int8_t>(delta_qp);
        (void)esp_h264_enc_hw_cfg_roi(static_cast<esp_h264_enc_param_hw_handle_t>(param_raw), roi);
    }

    esp_h264_enc_in_frame_t in_frame {};
    /* The encoder only reads the frame (cache write-back, then DMA). */
    in_frame.raw_data.buffer = const_cast<uint8_t *>(in.sdu.u8());
    in_frame.raw_data.len = static_cast<uint32_t>(in.sdu.size());
    in_frame.pts = static_cast<uint32_t>(in.ts_us / 1000);
    esp_h264_enc_out_frame_t out_frame {};
    out_frame.raw_data.buffer = slot_buf;
    out_frame.raw_data.len = slot_cap;

    const int64_t t0 = esp_timer_get_time();
    const esp_h264_err_t err = esp_h264_enc_process(enc, &in_frame, &out_frame);
    const int64_t dt = esp_timer_get_time() - t0;
    lock.lock();
    if (err != ESP_H264_ERR_OK || 0 == out_frame.length || out_frame.length > slot_cap)
    {
        pool->release(out_index);
        ++encode_errors_;
        /* Decoders never saw this frame, so the next one must not reference it. */
        pending_idr = true;
        return -EIO;
    }
    last_encode_us_ = dt;
    sum_encode_us_ += dt;
    max_encode_us_ = dt > max_encode_us_ ? dt : max_encode_us_;

    component_pdu au;
    au.ts_us = in.ts_us;
    au.sdu_type = sdu_type_e::H264_AU;
    au.port = 0;
    const bool idr = out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR;
    if (!idr)
    {
        sum_p_encode_us_ += dt;
        ++p_frames_;
    }
    if (frame_rc_now)
    {
        rc_qp_sum_ += static_cast<uint64_t>(qp_used);
        rc_update_locked(8.0 * out_frame.length, idr);
    }
    au.flags = idr ? static_cast<uint8_t>(pdu_flag_e::KEY) : 0;
    au.sdu = bfc::shared_sized_buffer::adopt(
        reinterpret_cast<std::byte *>(slot_buf), slot_cap, out_frame.length,
        [pool, out_index](std::byte *) { pool->release(out_index); });
    if (au.sdu.empty())
    {
        pool->release(out_index);
        return -ENOMEM;
    }
    const int64_t now_ns = steady_mono_ns();
    latency_store(latency_ms_, now_ns, static_cast<int64_t>(in.ts_us) * 1000);
    if (entry_ts_us_ == in.ts_us && 0 != entry_ns_)
    {
        latency_store(node_latency_ms_, now_ns, entry_ns_);
    }
    entry_ns_ = 0;
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
    std::lock_guard<std::mutex>  hw(hw_mu_);
    std::unique_lock<std::mutex> lock(mu);
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
    if (in.ts_us != entry_ts_us_ || 0 == entry_ns_)
    {
        entry_ts_us_ = in.ts_us;
        entry_ns_ = steady_mono_ns();
    }
    return encode_locked(in, lock);
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
        hw_dirty_ |= opened ? k_hw_fps : 0;
        return 0;
    }
    /* gop, bitrate and fps reach a live codec from the encode thread before its next frame. */
    if (key == "gop")
    {
        if (parse_int(value, 1, 255, &n) < 0)
        {
            return -EINVAL;
        }
        gop = static_cast<int>(n);
        hw_dirty_ |= opened ? k_hw_gop : 0;
        return 0;
    }
    if (key == "bitrate" || key == "bps")
    {
        if (parse_int(value, 10000, 100000000, &n) < 0)
        {
            return -EINVAL;
        }
        bitrate = static_cast<uint32_t>(n);
        hw_dirty_ |= opened ? k_hw_bitrate : 0;
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
    if (key == "rc")
    {
        if (value != "frame" && value != "esp")
        {
            return -EINVAL;
        }
        const bool f = value == "frame";
        reopen_req = reopen_req || (opened && f != frame_rc);
        frame_rc = f;
        return 0;
    }
    if (key == "reset_stats")
    {
        frames_ = 0;
        idr_frames_ = 0;
        encode_errors_ = 0;
        last_encode_us_ = 0;
        max_encode_us_ = 0;
        sum_encode_us_ = 0;
        sum_p_encode_us_ = 0;
        p_frames_ = 0;
        rc_qp_sum_ = 0;
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
    if (key == "enc_frames")
    {
        return set_int(value, static_cast<int64_t>(frames_));
    }
    if (key == "encode_ms")
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.1f/%.1f", last_encode_us_ / 1000.0,
                      max_encode_us_ / 1000.0);
        *value = buf;
        return 0;
    }
    if (const int r = latency_query(key, latency_ms_, node_latency_ms_, value); -ENOTSUP != r)
    {
        return r;
    }
    if (key == "stats")
    {
        char buf[160];
        const double avg_ms =
            frames_ > 0 ? static_cast<double>(sum_encode_us_) / static_cast<double>(frames_) / 1000.0
                        : 0.0;
        const double p_avg_ms =
            p_frames_ > 0 ? static_cast<double>(sum_p_encode_us_) / static_cast<double>(p_frames_) / 1000.0
                          : 0.0;
        std::snprintf(buf, sizeof(buf),
                      "frames=%" PRIu64 " idr=%" PRIu64 " errors=%" PRIu64
                      " encode_ms=%.1f avg=%.1f p_avg=%.1f max=%.1f qp=%d avg_qp=%.1f",
                      frames_, idr_frames_, encode_errors_, last_encode_us_ / 1000.0, avg_ms, p_avg_ms,
                      max_encode_us_ / 1000.0, frame_rc ? rc_qp_ : -1,
                      frame_rc && frames_ > 0 ? static_cast<double>(rc_qp_sum_) / frames_ : 0.0);
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
