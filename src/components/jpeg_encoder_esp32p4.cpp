#include "components/jpeg_encoder_esp32p4.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>

#include "core/key_util.hpp"
#include "core/sdu_type.hpp"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace vstreamer
{
namespace
{
constexpr size_t k_out_q_cap = 4;
constexpr int    k_engine_timeout_ms = 200;

int parse_int_in(std::string_view v, int64_t lo, int64_t hi, int *out)
{
    int64_t x = 0;
    if (key_parse_i64(v, &x) < 0 || x < lo || x > hi)
    {
        return -EINVAL;
    }
    *out = static_cast<int>(x);
    return 0;
}
}  // namespace

jpeg_encoder_esp32p4::jpeg_encoder_esp32p4() = default;

jpeg_encoder_esp32p4::~jpeg_encoder_esp32p4()
{
    close();
}

std::string jpeg_encoder_esp32p4::name() const
{
    return "jpeg_encoder_esp32p4";
}

int jpeg_encoder_esp32p4::open()
{
    std::lock_guard<std::mutex> hw(hw_mu_);
    std::lock_guard<std::mutex> lock(mu);
    if (opened_)
    {
        return 0;
    }
    jpeg_encode_engine_cfg_t ecfg {};
    ecfg.intr_priority = 0;
    ecfg.timeout_ms = k_engine_timeout_ms;
    jpeg_encoder_handle_t h = nullptr;
    if (const esp_err_t e = jpeg_new_encoder_engine(&ecfg, &h); e != ESP_OK)
    {
        std::printf("jpeg_encoder_esp32p4: engine: %s\n", esp_err_to_name(e));
        return e == ESP_ERR_NO_MEM ? -ENOMEM : -EIO;
    }
    engine_ = h;
    stop_ = false;
    have_caps_ = false;
    have_pending_ = false;
    busy_ = false;
    last_ts_us_ = -1;
    last_error_ = 0;
    opened_ = true;
    thread_opts opts = encode_place_.opts("vs_jpeg", 4 * 1024);
    opts.psram_stack = true; /* waits on the engine only; no flash or NVS access */
    worker_ = spawn_thread(opts, [this] { worker_main(); });
    return 0;
}

void jpeg_encoder_esp32p4::close()
{
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!opened_)
        {
            return;
        }
        opened_ = false;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    std::lock_guard<std::mutex> hw(hw_mu_);
    std::lock_guard<std::mutex> lock(mu);
    if (engine_)
    {
        jpeg_del_encoder_engine(static_cast<jpeg_encoder_handle_t>(engine_));
        engine_ = nullptr;
    }
    std::free(scratch_);
    scratch_ = nullptr;
    scratch_cap_ = 0;
    pending_ = {};
    have_pending_ = false;
    out_q_.clear();
    have_caps_ = false;
}

void jpeg_encoder_esp32p4::push_out_locked(component_pdu &&out)
{
    if (out_q_.size() >= k_out_q_cap)
    {
        /* Drop the oldest frame, never caps: the consumer needs them before the next frame. */
        for (auto it = out_q_.begin(); it != out_q_.end(); ++it)
        {
            if (it->sdu_type == sdu_type_e::MJPEG)
            {
                out_q_.erase(it);
                ++out_drops_;
                break;
            }
        }
    }
    out_q_.push_back(std::move(out));
}

int jpeg_encoder_esp32p4::input(component_pdu &&in)
{
    if (0 != in.port)
    {
        return -EINVAL;
    }
    if (in.sdu_type == sdu_type_e::CAPS_VIDEO_RAW)
    {
        video_raw_caps caps {};
        /* The engine's 2D DMA pads partial 16x16 MCUs at the edges (1080 lines is fine);
         * 4:2:0 chroma needs even sizes. */
        if (read_caps(in, &caps) != 0 || caps.width <= 0 || caps.height <= 0 ||
            caps.width % 2 != 0 || caps.height % 2 != 0)
        {
            return -ENOTSUP;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!opened_)
            {
                return -EBADF;
            }
            caps_ = caps;
            have_caps_ = true;
            video_coded_caps coded {};
            coded.width = caps.width;
            coded.height = caps.height;
            coded.fps_num = caps.fps_num;
            coded.fps_den = caps.fps_den;
            push_out_locked(make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, coded, in.ts_us, 0));
        }
        notify_output_ready();
        return 0;
    }
    if (in.sdu_type != sdu_type_e::O_UYY_E_VYY)
    {
        return -EINVAL;
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!opened_)
        {
            return -EBADF;
        }
        if (!have_caps_)
        {
            return -ENOTSUP;
        }
        const size_t want = static_cast<size_t>(caps_.width) * caps_.height * 3 / 2;
        if (in.sdu.size() < want)
        {
            return -EINVAL;
        }
        if (have_pending_ || busy_)
        {
            ++busy_drops_;
            return -EAGAIN;
        }
        const int64_t ts = static_cast<int64_t>(in.ts_us);
        /* 0.9 of the period: a camera a little off its nominal rate must not skip every
         * other frame. */
        if (max_fps_ > 0 && last_ts_us_ >= 0 && ts > last_ts_us_ &&
            ts - last_ts_us_ < 900000 / max_fps_)
        {
            ++rate_skips_;
            return 0;
        }
        last_ts_us_ = ts;
        pending_ = std::move(in);
        have_pending_ = true;
    }
    cv_.notify_one();
    return 0;
}

int jpeg_encoder_esp32p4::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened_)
    {
        return -EBADF;
    }
    if (out_q_.empty())
    {
        return -EAGAIN;
    }
    out = std::move(out_q_.front());
    out_q_.pop_front();
    return 0;
}

void jpeg_encoder_esp32p4::worker_main()
{
    for (;;)
    {
        component_pdu in;
        int           w = 0;
        int           h = 0;
        int           q = 0;
        {
            std::unique_lock<std::mutex> lock(mu);
            cv_.wait(lock, [this] { return stop_ || have_pending_; });
            if (stop_)
            {
                break;
            }
            in = std::move(pending_);
            pending_ = {};
            have_pending_ = false;
            busy_ = true;
            w = caps_.width;
            h = caps_.height;
            q = quality_;
        }
        int r = 0;
        {
            std::lock_guard<std::mutex> hw(hw_mu_);
            r = encode_one(std::move(in), w, h, q);
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            busy_ = false;
        }
        if (0 == r)
        {
            notify_output_ready();
        }
    }
}

int jpeg_encoder_esp32p4::encode_one(component_pdu &&in, int w, int h, int quality)
{
    /* hw_mu_ held. */
    const size_t need = outbuf_kb_ > 0 ? static_cast<size_t>(outbuf_kb_) * 1024
                                       : static_cast<size_t>(w) * h / 2;
    if (scratch_cap_ < need)
    {
        std::free(scratch_);
        scratch_ = nullptr;
        scratch_cap_ = 0;
        jpeg_encode_memory_alloc_cfg_t mcfg {};
        mcfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
        size_t got = 0;
        scratch_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(need, &mcfg, &got));
        if (!scratch_)
        {
            std::lock_guard<std::mutex> lock(mu);
            ++errors_;
            last_error_ = -ENOMEM;
            return -ENOMEM;
        }
        scratch_cap_ = got;
    }

    jpeg_encode_cfg_t cfg {};
    cfg.width = static_cast<uint32_t>(w);
    cfg.height = static_cast<uint32_t>(h);
    cfg.src_type = JPEG_ENCODE_IN_FORMAT_YUV420;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    cfg.image_quality = static_cast<uint32_t>(quality);

    const uint64_t ts_us = in.ts_us;
    const int64_t  t0 = esp_timer_get_time();
    uint32_t       n = 0;
    const esp_err_t e = jpeg_encoder_process(static_cast<jpeg_encoder_handle_t>(engine_), &cfg,
                                             in.sdu.u8(), static_cast<uint32_t>(in.sdu.size()),
                                             scratch_, static_cast<uint32_t>(scratch_cap_), &n);
    const int64_t dt = esp_timer_get_time() - t0;
    in = {}; /* the engine is done with the frame: back to capture */
    if (e != ESP_OK || 0 == n || n > scratch_cap_)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (n > scratch_cap_ || e == ESP_ERR_INVALID_SIZE)
        {
            ++overflows_;
        }
        else
        {
            ++errors_;
            last_error_ = -static_cast<int>(e);
        }
        return -EIO;
    }

    auto *p = static_cast<std::byte *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p)
    {
        std::lock_guard<std::mutex> lock(mu);
        ++errors_;
        last_error_ = -ENOMEM;
        return -ENOMEM;
    }
    std::memcpy(p, scratch_, n);

    component_pdu out;
    out.ts_us = ts_us;
    out.sdu_type = sdu_type_e::MJPEG;
    out.port = 0;
    out.flags = static_cast<uint8_t>(pdu_flag_e::KEY) | static_cast<uint8_t>(pdu_flag_e::AU_END);
    out.sdu = bfc::shared_sized_buffer::adopt(p, n, n, [](std::byte *b) { heap_caps_free(b); });

    std::lock_guard<std::mutex> lock(mu);
    out.seq = out_seq_++;
    ++frames_;
    bytes_ += n;
    enc_us_sum_ += dt;
    if (dt > enc_us_max_)
    {
        enc_us_max_ = dt;
    }
    push_out_locked(std::move(out));
    return 0;
}

int jpeg_encoder_esp32p4::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(mu);
    if (const int r = thread_place_configure(&encode_place_, "encode", key, value, opened_);
        r != -ENOTSUP)
    {
        return r;
    }
    if (key == "quality")
    {
        return parse_int_in(value, 1, 100, &quality_);
    }
    if (key == "max_fps")
    {
        return parse_int_in(value, 0, 240, &max_fps_);
    }
    if (key == "outbuf_kb")
    {
        if (opened_)
        {
            return -EBUSY;
        }
        return parse_int_in(value, 0, 16 * 1024, &outbuf_kb_);
    }
    if (key == "reset_stats")
    {
        frames_ = busy_drops_ = rate_skips_ = errors_ = overflows_ = out_drops_ = bytes_ = 0;
        enc_us_sum_ = enc_us_max_ = 0;
        last_error_ = 0;
        return 0;
    }
    return -ENOTSUP;
}

int jpeg_encoder_esp32p4::query(std::string_view key, std::string *value) const
{
    if (!value)
    {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> lock(mu);
    if (const int r = thread_place_query(encode_place_, "encode", key, value); r != -ENOTSUP)
    {
        return r;
    }
    if (key == "quality" || key == "max_fps" || key == "outbuf_kb")
    {
        const int v = key == "quality" ? quality_ : key == "max_fps" ? max_fps_ : outbuf_kb_;
        *value = std::to_string(v);
        return 0;
    }
    if (key == "state" || key == "status")
    {
        *value = !opened_ ? "closed" : last_error_ ? "error" : "open";
        return 0;
    }
    if (key == "backend")
    {
        *value = "esp_driver_jpeg";
        return 0;
    }
    if (key == "stats")
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "frames=%" PRIu64 " busy_drops=%" PRIu64 " rate_skips=%" PRIu64
                      " out_drops=%" PRIu64 " overflows=%" PRIu64 " errors=%" PRIu64
                      " enc_ms_avg=%.1f enc_ms_max=%.1f kb_avg=%.0f quality=%d error=%d",
                      frames_, busy_drops_, rate_skips_, out_drops_, overflows_, errors_,
                      frames_ ? enc_us_sum_ / 1000.0 / frames_ : 0.0, enc_us_max_ / 1000.0,
                      frames_ ? bytes_ / 1024.0 / frames_ : 0.0, quality_, last_error_);
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
