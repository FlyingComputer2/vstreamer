#include "components/esp32p4_csi_camera_src.hpp"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <optional>
#include <utility>

#include "components/esp32p4_csi_device.hpp"
#include "core/latency_query.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/time_util.hpp"
#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif
namespace vstreamer
{
namespace
{
const std::vector<port_desc> &output_ports()
{
    static const std::vector<port_desc> ports = []
    {
        port_desc p;
        port_caps_entry caps{};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_RAW;
        p.caps.push_back(caps);
        port_caps_entry data{};
        data.sdu_type = sdu_type_e::O_UYY_E_VYY;
        p.caps.push_back(data);
        return std::vector<port_desc>{p};
    }();
    return ports;
}

}  // namespace
esp32p4_csi_camera_src::esp32p4_csi_camera_src()
    : device_(std::make_unique<detail::esp32p4_csi_device>())
{
}
esp32p4_csi_camera_src::~esp32p4_csi_camera_src()
{
    close();
}
std::string esp32p4_csi_camera_src::name() const
{
    return "esp32p4_csi_camera_src";
}

void esp32p4_csi_camera_src::capture_main()
{
    while (!stop_.load(std::memory_order_acquire))
    {
        component_pdu pdu;
        const int result = device_->dequeue(pdu);
        if (result != 0)
        {
            if (result != -EAGAIN)
            {
                {
                    std::lock_guard<std::mutex> lock(mu);
                    ++dqbuf_errors_;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }
        const uint64_t ts_us = pdu.ts_us;
        std::optional<component_pdu> stale;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (ready_q_.size() >= k_ready_q_max)
            {
                stale = std::move(ready_q_.front());
                ready_q_.pop_front();
                ++dropped_;
            }
            ready_q_.push_back(std::move(pdu));
            ++frames_;
            if (frames_ > 1)
            {
                sum_dq_interval_us_ += ts_us - last_dq_ts_us_;
                ++dq_interval_samples_;
            }
            last_dq_ts_us_ = ts_us;
        }
        /* stale's buffer is requeued here, outside mu. */
        stale.reset();
        notify_wakeup();
        out_cv_.notify_one();
#if defined(ESP_PLATFORM)
        {
            /* Under mu: set_wake_task(nullptr) runs on the woken task before it
             * exits, so the handle is alive for as long as it is set. */
            std::lock_guard<std::mutex> lock(mu);
            if (nullptr != wake_task_)
            {
                xTaskNotifyGive(static_cast<TaskHandle_t>(wake_task_));
            }
        }
#endif
    }
}

#if defined(ESP_PLATFORM)
void esp32p4_csi_camera_src::set_wake_task(void *task_handle)
{
    std::lock_guard<std::mutex> lock(mu);
    wake_task_ = task_handle;
}
#endif

int esp32p4_csi_camera_src::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (source_open)
    {
        return 0;
    }
    const int r = device_->open();
    if (r < 0)
    {
        return r;
    }
    source_open = true;
    caps_sent_ = false;
    stop_.store(false, std::memory_order_release);
    /* Queue publication by capture_main() is serialized by mu. */
    thread_ = spawn_thread(capture_place_.opts("vs_csi_cam", 8 * 1024), [this] { capture_main(); });
    return 0;
}

void esp32p4_csi_camera_src::close()
{
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!source_open)
        {
            return;
        }
        source_open = false;
#if defined(ESP_PLATFORM)
        wake_task_ = nullptr;
#endif
        out_cv_.notify_all();
    }
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable())
    {
        thread_.join();
    }
    std::deque<component_pdu> pending;
    {
        std::lock_guard<std::mutex> lock(mu);
        pending.swap(ready_q_);
    }
    pending.clear();
    std::lock_guard<std::mutex> lock(mu);
    device_->close();
}

int esp32p4_csi_camera_src::try_pop_output_locked(component_pdu &out)
{
    if (!source_open)
    {
        return -EBADF;
    }
    if (!caps_sent_)
    {
        video_raw_caps caps{};
        caps.width = device_->live_width();
        caps.height = device_->live_height();
        caps.hor_stride = device_->live_width();
        caps.ver_stride = device_->live_height();
        caps.fps_num = device_->frame_rate();
        caps.fps_den = 1;
        out = make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps,
                            static_cast<uint64_t>(steady_mono_ns() / 1000), 0);
        out.seq = out_seq_++;
        caps_sent_ = true;
        return 0;
    }
    if (ready_q_.empty())
    {
        return -EAGAIN;
    }
    out = std::move(ready_q_.front());
    ready_q_.pop_front();
    out.seq = out_seq_++;
    latency_store(latency_ms_, steady_mono_ns(), static_cast<int64_t>(out.ts_us) * 1000);
    return 0;
}

int esp32p4_csi_camera_src::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu);
    return try_pop_output_locked(out);
}

int esp32p4_csi_camera_src::wait_output(component_pdu &out, int timeout_ms)
{
    if (timeout_ms < 0)
    {
        timeout_ms = 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mu);
    for (;;)
    {
        const int r = try_pop_output_locked(out);
        if (0 == r || -EBADF == r)
        {
            return r;
        }
        if (-EAGAIN != r)
        {
            return r;
        }
        if (!source_open || stop_.load(std::memory_order_acquire))
        {
            return -EBADF;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return -EAGAIN;
        }
#if defined(ESP_PLATFORM)
        if (nullptr != wake_task_)
        {
            /* Whatever is left of timeout_ms, so a stalled sensor still returns
             * -EAGAIN. */
            const auto left_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1;
            lock.unlock();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left_ms));
            lock.lock();
            continue;
        }
#endif
        out_cv_.wait_until(
            lock, deadline, [this]
            { return !ready_q_.empty() || !source_open || stop_.load(std::memory_order_acquire); });
        if (ready_q_.empty() && source_open && !stop_.load(std::memory_order_acquire))
        {
            return -EAGAIN;
        }
    }
}

int esp32p4_csi_camera_src::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(mu);
    if (const int pr = thread_place_configure(&capture_place_, "capture", key, value, source_open);
        -ENOTSUP != pr)
    {
        return pr;
    }
    if (source_open && key != "fps")
    {
        return -EBUSY;
    }
    return device_->configure(key, value);
}

int esp32p4_csi_camera_src::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    if (const int pr = thread_place_query(capture_place_, "capture", key, value); -ENOTSUP != pr)
    {
        return pr;
    }
    const int r = port_caps_query(output_ports(), false, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }

    if (const int lr = latency_query(key, latency_ms_, latency_ms_, value); -ENOTSUP != lr)
    {
        return lr;
    }
    std::lock_guard<std::mutex> lock(mu);
    if (key == "state" || key == "status")
    {
        *value = source_open ? "capturing" : "closed";
        return 0;
    }
    if (key == "stats")
    {
        char buf[128];
        const double dq_ms = dq_interval_samples_ > 0
                                 ? static_cast<double>(sum_dq_interval_us_) /
                                       static_cast<double>(dq_interval_samples_) / 1000.0
                                 : 0.0;
        std::snprintf(buf, sizeof(buf),
                      "frames=%" PRIu64 " dropped=%" PRIu64 " dqbuf_errors=%" PRIu64 " dq_ms=%.2f",
                      frames_, dropped_, dqbuf_errors_, dq_ms);
        *value = buf;
        return 0;
    }
    return device_->query(key, value);
}

}  // namespace vstreamer
