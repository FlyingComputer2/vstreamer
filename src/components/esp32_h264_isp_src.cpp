#include "components/esp32_h264_isp_src.hpp"

#include "components/esp32p4_csi_camera_src.hpp"
#include "components/h264_encoder_esp32p4.hpp"

#include "core/key_util.hpp"
#include "core/latency_query.hpp"
#include "core/time_util.hpp"
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
#include <string>
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

void esp32_h264_isp_src::drain_encoder_output()
{
    component_pdu out;
    while (enc_->output(out) == 0)
    {
        std::lock_guard<std::mutex> lock(mu);
        /* Never drop a coded AU (the next P frames reference it); encode_main keeps the backlog
         * at k_coded_backlog_max, far below this. */
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
        std::unique_lock<std::mutex> lock(mu);
        out_cv_.wait_for(lock, std::chrono::milliseconds(1),
                         [&] { return out_q_.size() < k_out_q_max / 2U || stop_.load(); });
    }
    return -EAGAIN;
}

void esp32_h264_isp_src::encode_main()
{
    /* The camera wakes this task with a notification instead of its condition variable. */
    cam_->set_wake_task(xTaskGetCurrentTaskHandle());
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
        if (in.sdu_type == sdu_type_e::O_UYY_E_VYY)
        {
            std::lock_guard<std::mutex> lock(mu);
            if (out_q_.size() >= k_coded_backlog_max)
            {
                ++backlog_drops_;
                continue;
            }
        }

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
    /* Before this task is gone: the camera notifies it under its lock while it is set. */
    cam_->set_wake_task(nullptr);
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

    int qmin = 0;
    int qmax = 0;
    char buf[16];
    {
        std::lock_guard<std::mutex> lock(mu);
        if (qp_min > qp_max)
        {
            return -EINVAL;
        }
        qmin = qp_min;
        qmax = qp_max;
        std::snprintf(buf, sizeof(buf), "%d", buffers);
    }
    if (cam_->configure("buffers", buf) < 0)
    {
        return -EINVAL;
    }
    std::snprintf(buf, sizeof(buf), "%d", fps);
    if (cam_->configure("fps", buf) < 0)
    {
        return -EINVAL;
    }

    const int cr = cam_->open();
    if (cr < 0)
    {
        return cr;
    }

    /* Each key must take: a rejected one would leave the encoder at its own default. qp_min goes
     * to 0 first so that the qp_max/qp_min pair never crosses the encoder's current range. */
    std::string size;
    cam_->query("size", &size);
    const auto num = [](int64_t v) { return std::to_string(v); };
    const std::pair<const char *, std::string> enc_cfg[] = {
        {"size", size},          {"bitrate", num(bitrate)}, {"gop", num(gop)},
        {"fps", num(fps)},       {"qp_min", "0"},           {"qp_max", num(qmax)},
        {"qp_min", num(qmin)},
    };
    for (const auto &[key, value] : enc_cfg)
    {
        if (enc_->configure(key, value) < 0)
        {
            std::fprintf(stderr, "esp32_h264_isp_src: encoder rejected %s=%s\n", key,
                         value.c_str());
            cam_->close();
            return -EINVAL;
        }
    }

    if (enc_->open() < 0)
    {
        cam_->close();
        return -EIO;
    }

    stop_.store(false, std::memory_order_release);
    std::thread enc_thread =
        spawn_thread(encode_place_.opts("vs_isp_enc", k_pipeline_stack), [this] { encode_main(); });
    {
        std::lock_guard<std::mutex> lock(mu);
        encode_thread_ = std::move(enc_thread);
        opened = true;
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
    if (out.sdu_type == sdu_type_e::H264_AU)
    {
        latency_store(latency_ms_, steady_mono_ns(), static_cast<int64_t>(out.ts_us) * 1000);
    }
    return 0;
}

int esp32_h264_isp_src::configure(std::string_view key, std::string_view value)
{
    /* capture_cpu / capture_prio fall through to the camera below. */
    if (const int pr = thread_place_configure(&encode_place_, "encode", key, value); -ENOTSUP != pr)
    {
        return pr;
    }
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
        /* esp_h264 stores the GOP in 8 bits. */
        if (key_parse_i64(value, &n) < 0 || n < 1 || n > 255)
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
    if (const int pr = thread_place_query(encode_place_, "encode", key, value); -ENOTSUP != pr)
    {
        return pr;
    }
    int r = port_caps_query(output_ports(), false, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }
    if (const int lr = latency_query(key, latency_ms_, latency_ms_, value); -ENOTSUP != lr)
    {
        return lr;
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
        std::snprintf(buf, sizeof(buf),
                      "cam %s; enc %s; wall_ms=%.1f wait_ms=%.1f push_ms=%.1f backlog_drops=%" PRIu64,
                      cam_stats.c_str(), enc_stats.c_str(), wall_ms, wait_ms, push_ms,
                      backlog_drops_);
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
