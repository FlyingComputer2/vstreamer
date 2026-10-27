#include "components/esp32_h264_isp_ng_src.hpp"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <utility>

#include "components/esp32p4_csi_device.hpp"
#include "components/h264_encoder_esp32p4.hpp"
#include "core/latency_query.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/time_util.hpp"

namespace vstreamer
{
namespace
{
const std::vector<port_desc> &output_ports()
{
    static const std::vector<port_desc> ports = []
    {
        port_desc p;
        port_caps_entry caps{}, au{};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_CODED;
        au.sdu_type = sdu_type_e::H264_AU;
        p.caps.push_back(caps);
        p.caps.push_back(au);
        return std::vector<port_desc>{p};
    }();
    return ports;
}
bool camera_key(std::string_view key)
{
    return key == "size" || key == "fps" || key == "buffers" || key == "light_freq" ||
           key == "sccb_port" || key == "sccb_scl" || key == "sccb_sda" || key == "sccb_freq" ||
           key == "reset_pin" || key == "pwdn_pin";
}
bool encoder_key(std::string_view key)
{
    return key == "bitrate" || key == "bps" || key == "gop" || key == "qp_min" || key == "qp_max" ||
           key == "rc" || key == "idr";
}
}  // namespace

esp32_h264_isp_ng_src::esp32_h264_isp_ng_src()
    : camera_(std::make_unique<detail::esp32p4_csi_device>()),
      encoder_(std::make_unique<h264_encoder_esp32p4>())
{
}
esp32_h264_isp_ng_src::~esp32_h264_isp_ng_src()
{
    close();
}
std::string esp32_h264_isp_ng_src::name() const
{
    return "esp32_h264_isp_ng_src";
}

int esp32_h264_isp_ng_src::open()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    std::lock_guard<std::mutex> lock(mu_);
    if (opened_)
    {
        return 0;
    }
    int r = camera_->open();
    if (r < 0)
    {
        return r;
    }
    std::string size, fps;
    camera_->query("size", &size);
    camera_->query("fps", &fps);
    r = encoder_->configure("size", size);
    if (r == 0)
    {
        r = encoder_->configure("fps", fps);
    }
    if (r == 0)
    {
        r = encoder_->open();
    }
    if (r == 0)
    {
        video_raw_caps caps{};
        caps.width = camera_->live_width();
        caps.height = camera_->live_height();
        caps.hor_stride = caps.width;
        caps.ver_stride = caps.height;
        caps.fps_num = camera_->frame_rate();
        caps.fps_den = 1;
        r = encoder_->input(
            make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, steady_mono_ns() / 1000, 0));
    }
    if (r < 0)
    {
        encoder_->close();
        camera_->close();
        return r;
    }
    frames_ = backlog_drops_ = dqbuf_errors_ = encode_errors_ = 0;
    last_error_ = 0;
    latency_ms_.store(-1.0, std::memory_order_relaxed);
    stop_.store(false, std::memory_order_release);
    opened_ = true;
    worker_ =
        spawn_thread(encode_place_.opts("vs_isp_direct", 24 * 1024), [this] { encode_main(); });
    return 0;
}

void esp32_h264_isp_ng_src::close()
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    {
        std::lock_guard<std::mutex> lock(mu_);
        opened_ = false;
        stop_.store(true, std::memory_order_release);
    }
    notify_wakeup();
    // DQBUF has a 100 ms driver timeout. No mappings are freed during encode.
    if (worker_.joinable())
    {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mu_);
    encoder_->close();
    out_q_.clear();
    camera_->close();
}

void esp32_h264_isp_ng_src::drain_encoder()
{
    component_pdu out;
    bool queued = false;
    while (encoder_->output(out) == 0)
    {
        std::lock_guard<std::mutex> lock(mu_);
        out_q_.push_back(std::move(out));
        queued = true;
    }
    if (queued)
    {
        notify_wakeup();
    }
}

void esp32_h264_isp_ng_src::encode_main()
{
    drain_encoder();  // Publish coded caps even before the first captured
                      // frame.
    while (!stop_.load(std::memory_order_acquire))
    {
        component_pdu frame;
        const int r = camera_->dequeue(frame);
        if (r < 0)
        {
            if (r != -EAGAIN)
            {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    ++dqbuf_errors_;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }
        if (stop_.load(std::memory_order_acquire))
        {
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++frames_;
            // Drain capture while the consumer is slow, dropping raw frames
            // only. Never discard a coded AU: later P frames can reference it.
            // One AU may still wait: encode takes about a frame interval, so
            // the next frame is ready as soon as an AU is queued, before a
            // polling consumer has taken it.
            if (out_q_.size() >= 2)
            {
                ++backlog_drops_;
                continue;
            }
        }
        // input() reads this exact camera mapping through encoder DMA. Its
        // synchronous return ends DMA use; the PDU release requeues the buffer.
        const int enc_r = encoder_->input(std::move(frame));
        frame.sdu = {};
        drain_encoder();
        if (enc_r == -EAGAIN)
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++backlog_drops_;  // All coded-buffer slots are still held
                               // downstream.
        }
        else if (enc_r < 0)
        {
            {
                std::lock_guard<std::mutex> lock(mu_);
                ++encode_errors_;
                if (enc_r != -EIO)
                {
                    last_error_ = enc_r;
                }
            }
            if (enc_r != -EIO)
            {
                notify_wakeup();
                break;
            }
            // Encoder recovers a bitstream overflow by requesting a fresh IDR.
        }
    }
}

int esp32_h264_isp_ng_src::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (!opened_)
    {
        return -EBADF;
    }
    if (out_q_.empty())
    {
        return last_error_ ? last_error_ : -EAGAIN;
    }
    out = std::move(out_q_.front());
    out_q_.pop_front();
    if (out.sdu_type == sdu_type_e::H264_AU)
    {
        latency_store(latency_ms_, steady_mono_ns(), static_cast<int64_t>(out.ts_us) * 1000);
    }
    return 0;
}

int esp32_h264_isp_ng_src::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    std::unique_lock<std::mutex> lock(mu_);
    if (const int r = thread_place_configure(&encode_place_, "encode", key, value, opened_);
        r != -ENOTSUP)
    {
        return r;
    }
    if (key == "reset_stats")
    {
        frames_ = backlog_drops_ = dqbuf_errors_ = encode_errors_ = 0;
        lock.unlock();
        return encoder_->configure(key, value);
    }
    if (!encoder_key(key) && !camera_key(key))
    {
        return -ENOTSUP;
    }
    // Reopening the encoder with live AUs can exhaust PSRAM. Only settings that
    // esp_h264 updates in place are accepted while streaming.
    if (opened_ && key != "bitrate" && key != "bps" && key != "gop" && key != "idr")
    {
        return -EBUSY;
    }
    if (encoder_key(key))
    {
        // Encoder control takes the encoder's own lock; leave output() free to run.
        lock.unlock();
        return encoder_->configure(key, value);
    }
    return camera_->configure(key, value);
}

int esp32_h264_isp_ng_src::query(std::string_view key, std::string *value) const
{
    if (!value)
    {
        return -EINVAL;
    }
    std::unique_lock<std::mutex> lock(mu_);
    if (const int r = thread_place_query(encode_place_, "encode", key, value); r != -ENOTSUP)
    {
        return r;
    }
    if (const int r = port_caps_query(output_ports(), false, key, value); r != -ENOTSUP)
    {
        return r;
    }
    if (const int r = latency_query(key, latency_ms_, latency_ms_, value); r != -ENOTSUP)
    {
        return r;
    }
    if (key == "state" || key == "status")
    {
        *value = !opened_ ? "closed" : last_error_ ? "error" : "open";
        return 0;
    }
    if (key == "backend")
    {
        *value = "esp_video_csi+esp_h264_hw_direct";
        return 0;
    }
    if (key == "stats")
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "frames=%" PRIu64 " backlog_drops=%" PRIu64 " dqbuf_errors=%" PRIu64
                      " encode_errors=%" PRIu64 " error=%d; enc ",
                      frames_, backlog_drops_, dqbuf_errors_, encode_errors_, last_error_);
        // Snapshot our counters before waiting on the encoder's encode mutex.
        lock.unlock();
        std::string enc;
        encoder_->query("stats", &enc);
        *value = std::string(buf) + enc;
        return 0;
    }
    const int r = camera_->query(key, value);
    lock.unlock();
    return r == -ENOTSUP ? encoder_->query(key, value) : r;
}
}  // namespace vstreamer
