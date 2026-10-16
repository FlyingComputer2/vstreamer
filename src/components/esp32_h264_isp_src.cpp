#include "components/esp32_h264_isp_src.hpp"

#include "components/esp32p4_csi_camera_src.hpp"
#include "components/h264_encoder_esp32p4.hpp"

#include "core/key_util.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/thread_spawn.hpp"

#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <chrono>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <thread>
#include <utility>

namespace vstreamer
{
namespace
{

constexpr size_t k_pipeline_stack = 24 * 1024;

}  // namespace

const std::vector<port_desc> &esp32_h264_isp_src::output_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc       p;
        port_caps_entry coded {};
        coded.sdu_type = sdu_type_e::CAPS_VIDEO_CODED;
        p.caps.push_back(coded);
        port_caps_entry au {};
        au.sdu_type = sdu_type_e::H264_AU;
        p.caps.push_back(au);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

esp32_h264_isp_src::esp32_h264_isp_src()
    : cam_(std::make_unique<esp32p4_csi_camera_src>())
    , enc_(std::make_unique<h264_encoder_esp32p4>())
{
}

esp32_h264_isp_src::~esp32_h264_isp_src()
{
    close();
}

std::string esp32_h264_isp_src::name() const
{
    return "esp32_h264_isp_src";
}

void esp32_h264_isp_src::set_live_tx_hooks(coded_sink_fn sink, wire_drain_fn drain)
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return;
    }
    coded_sink_ = std::move(sink);
    wire_drain_ = std::move(drain);
}

void esp32_h264_isp_src::drain_encoder_output()
{
    component_pdu out;
    while (enc_->output(out) == 0)
    {
        if (coded_sink_)
        {
            if (!coded_sink_(std::move(out)))
            {
                stop_.store(true, std::memory_order_release);
                return;
            }
            continue;
        }
        std::lock_guard<std::mutex> lock(mu);
        if (out_q_.size() >= k_out_q_max)
        {
            out_q_.pop_front();
        }
        out_q_.push_back(std::move(out));
        out_cv_.notify_all();
        notify_wakeup();
    }
}

int esp32_h264_isp_src::push_encoder_input(component_pdu &in)
{
    constexpr int k_max_backpressure = 2000;
    for (int attempt = 0; attempt < k_max_backpressure; ++attempt)
    {
        if (stop_.load(std::memory_order_acquire))
        {
            return -ECANCELED;
        }
        drain_encoder_output();
        component_pdu work = in;
        const int       r = enc_->input(std::move(work));
        if (-EAGAIN != r)
        {
            if (0 == r)
            {
                in.sdu = {};
                drain_encoder_output();
            }
            return r;
        }
        notify_wakeup();
        if (wire_drain_)
        {
            wire_drain_();
        }
        if (coded_sink_)
        {
            vTaskDelay(1);
            continue;
        }
        std::unique_lock<std::mutex> lock(mu);
        out_cv_.wait_for(lock, std::chrono::milliseconds(1),
                         [&] { return out_q_.size() < k_out_q_max / 2U || stop_.load(); });
    }
    return -EAGAIN;
}

void esp32_h264_isp_src::encode_main()
{
    while (!stop_.load(std::memory_order_acquire))
    {
        const int64_t t_wait0 = esp_timer_get_time();
        component_pdu in;
        const int       pull_r = cam_->wait_output(in, 100);
        const int64_t t_wait1 = esp_timer_get_time();
        if (pull_r != 0)
        {
            continue;
        }
        if (in.sdu_type != sdu_type_e::CAPS_VIDEO_RAW && in.sdu_type != sdu_type_e::O_UYY_E_VYY)
        {
            continue;
        }
        wait_sum_us_ += t_wait1 - t_wait0;
        ++wait_samples_;

        const int64_t t_push0 = esp_timer_get_time();
        const int   enc_r = push_encoder_input(in);
        const int64_t t_push1 = esp_timer_get_time();
        if (-EIO == enc_r)
        {
            /* One frame lost (e.g. bitstream overflow); the encoder restarts on an IDR. */
            std::fprintf(stderr, "esp32_h264_isp_src: encode failed, frame dropped\n");
            continue;
        }
        if (enc_r < 0)
        {
            std::fprintf(stderr, "esp32_h264_isp_src: encode failed: %d\n", enc_r);
            break;
        }
        push_sum_us_ += t_push1 - t_push0;
        ++push_samples_;

        if (in.sdu_type == sdu_type_e::O_UYY_E_VYY)
        {
            const int64_t t_done = t_push1;
            if (last_wall_us_ > 0)
            {
                wall_sum_us_ += t_done - last_wall_us_;
                ++wall_frames_;
            }
            last_wall_us_ = t_done;
        }
    }
}

int esp32_h264_isp_src::open()
{
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return 0;
        }
    }

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", buffers);
    (void)cam_->configure("buffers", buf);
    std::snprintf(buf, sizeof(buf), "%d", fps);
    (void)cam_->configure("fps", buf);

    const int cr = cam_->open();
    if (cr < 0)
    {
        return cr;
    }

    std::string size;
    cam_->query("size", &size);
    enc_->configure("size", size);
    std::snprintf(buf, sizeof(buf), "%" PRIu32, bitrate);
    enc_->configure("bitrate", buf);
    std::snprintf(buf, sizeof(buf), "%d", gop);
    enc_->configure("gop", buf);
    std::snprintf(buf, sizeof(buf), "%d", fps);
    enc_->configure("fps", buf);
    std::snprintf(buf, sizeof(buf), "%d", qp_min);
    enc_->configure("qp_min", buf);
    std::snprintf(buf, sizeof(buf), "%d", qp_max);
    enc_->configure("qp_max", buf);

    if (enc_->open() < 0)
    {
        cam_->close();
        return -EIO;
    }

    stop_.store(false, std::memory_order_release);
    std::thread enc_thread =
        spawn_thread(thread_opts {"vs_isp_enc", k_pipeline_stack, 1, 13}, [this] { encode_main(); });
    {
        std::lock_guard<std::mutex> lock(mu);
        encode_thread_ = std::move(enc_thread);
        opened = true;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    if (TaskHandle_t enc_task = xTaskGetHandle("vs_isp_enc"); nullptr != enc_task)
    {
        cam_->set_wake_task(enc_task);
    }
    return 0;
}

void esp32_h264_isp_src::close()
{
    stop_.store(true, std::memory_order_release);
    out_cv_.notify_all();
    notify_wakeup();
    if (encode_thread_.joinable())
    {
        encode_thread_.join();
    }
    std::lock_guard<std::mutex> lock(mu);
    if (enc_)
    {
        enc_->close();
    }
    if (cam_)
    {
        cam_->close();
    }
    out_q_.clear();
    coded_sink_ = nullptr;
    wire_drain_ = nullptr;
    opened = false;
    stop_.store(false, std::memory_order_release);
}

int esp32_h264_isp_src::output(component_pdu &out)
{
    std::unique_lock<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }
    if (out_q_.empty())
    {
        out_cv_.wait_for(lock, std::chrono::milliseconds(50),
                         [&] { return !out_q_.empty() || !opened; });
    }
    if (out_q_.empty())
    {
        return -EAGAIN;
    }
    out = std::move(out_q_.front());
    out_q_.pop_front();
    return 0;
}

int esp32_h264_isp_src::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return -EBUSY;
    }
    if (key == "fps")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 1 || n > 120)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(n);
        return 0;
    }
    if (key == "gop")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 1 || n > 300)
        {
            return -EINVAL;
        }
        gop = static_cast<int>(n);
        return 0;
    }
    if (key == "bitrate" || key == "bps")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 25000)
        {
            return -EINVAL;
        }
        bitrate = static_cast<uint32_t>(n);
        return 0;
    }
    if (key == "qp_min")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 0 || n > 51)
        {
            return -EINVAL;
        }
        qp_min = static_cast<int>(n);
        return 0;
    }
    if (key == "qp_max")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 0 || n > 51)
        {
            return -EINVAL;
        }
        qp_max = static_cast<int>(n);
        return 0;
    }
    if (key == "buffers")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 2 || n > 8)
        {
            return -EINVAL;
        }
        buffers = static_cast<int>(n);
        return 0;
    }
    return cam_->configure(key, value);
}

int esp32_h264_isp_src::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    int r = port_caps_query(output_ports(), false, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }
    if (key == "status")
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = opened ? "open" : "closed";
        return 0;
    }
    if (key == "backend")
    {
        *value = "esp_video_csi+esp_h264_hw";
        return 0;
    }
    if (key == "stats")
    {
        std::string cam_stats;
        std::string enc_stats;
        cam_->query("stats", &cam_stats);
        enc_->query("stats", &enc_stats);
        char buf[384];
        const double wall_ms =
            wall_frames_ > 0 ? static_cast<double>(wall_sum_us_) / static_cast<double>(wall_frames_) / 1000.0
                             : 0.0;
        const double wait_ms =
            wait_samples_ > 0 ? static_cast<double>(wait_sum_us_) / static_cast<double>(wait_samples_) / 1000.0
                              : 0.0;
        const double push_ms =
            push_samples_ > 0 ? static_cast<double>(push_sum_us_) / static_cast<double>(push_samples_) / 1000.0
                              : 0.0;
        std::snprintf(buf, sizeof(buf), "cam %s; enc %s; wall_ms=%.1f wait_ms=%.1f push_ms=%.1f",
                      cam_stats.c_str(), enc_stats.c_str(), wall_ms, wait_ms, push_ms);
        *value = buf;
        return 0;
    }
    if (key == "encode_ms" || key == "enc_frames")
    {
        return enc_->query(key, value);
    }
    return cam_->query(key, value);
}

}  // namespace vstreamer
