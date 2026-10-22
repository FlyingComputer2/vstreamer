#ifndef VSTREAMER_COMPONENTS_ESP32P4_CSI_CAMERA_SRC_HPP
#define VSTREAMER_COMPONENTS_ESP32P4_CSI_CAMERA_SRC_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_ESP32P4_CSI_CAMERA_SRC
#error "esp32p4_csi_camera_src requires ENABLE_ESP32P4_CSI_CAMERA_SRC (ESP-IDF, esp32p4 target)"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "core/component_pdu.hpp"
#include "core/component_source.hpp"
#include "core/thread_spawn.hpp"

namespace vstreamer
{
namespace detail
{
class esp32p4_csi_device;
}

/*
 * MIPI-CSI camera → ISP → O_UYY_E_VYY on the ESP32-P4 (esp_video, /dev/video0).
 * Frames are the driver's own buffers, not copies: each one goes back to the
 * driver when the last reference to its PDU drops, so a consumer that holds
 * frames stalls capture. A capture thread keeps only the newest frame; output()
 * hands it out after CAPS_VIDEO_RAW.
 *
 * esp_video_init() runs once per boot on the first open(); the sccb_* / *_pin
 * keys only matter before that. The sensor and its mode come from sdkconfig
 * (esp_cam_sensor).
 */
class esp32p4_csi_camera_src : public component_source
{
   public:
    esp32p4_csi_camera_src();
    ~esp32p4_csi_camera_src() override;

    esp32p4_csi_camera_src(const esp32p4_csi_camera_src &) = delete;
    esp32p4_csi_camera_src &operator=(const esp32p4_csi_camera_src &) = delete;

    [[nodiscard]] std::string name() const override;
    int open() override;
    void close() override;
    int output(component_pdu &out) override;
    /* Blocks up to timeout_ms for a frame (or CAPS once). Returns -EAGAIN on
     * timeout. */
    int wait_output(component_pdu &out, int timeout_ms);

#if defined(ESP_PLATFORM)
    /* Wake vs_isp_enc via xTaskNotify when a frame lands in ready_q_ (lower
     * latency than cv alone). */
    void set_wake_task(void *task_handle);
#endif

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

   private:
    void capture_main();
    int try_pop_output_locked(component_pdu &out);
    std::unique_ptr<detail::esp32p4_csi_device> device_;
    mutable std::mutex mu;
    std::condition_variable out_cv_;

    bool source_open = false;
    std::thread thread_;
    thread_place capture_place_; /* keys capture_cpu / capture_prio */
    std::atomic<bool> stop_{false};
    /* Standard latency queries: DQBUF to output() of the last frame (the same
     * for a source). */
    std::atomic<double> latency_ms_{-1.0};
    static constexpr size_t k_ready_q_max = 1; /* newest frame only: older ones are latency */
    std::deque<component_pdu> ready_q_;
#if defined(ESP_PLATFORM)
    void *wake_task_ = nullptr;
#endif
    bool caps_sent_ = false;
    uint64_t out_seq_ = 0;

    uint64_t frames_ = 0;
    uint64_t dropped_ = 0; /* replaced in ready_ before output() took them */
    uint64_t dqbuf_errors_ = 0;
    uint64_t last_dq_ts_us_ = 0;
    uint64_t sum_dq_interval_us_ = 0;
    uint64_t dq_interval_samples_ = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_ESP32P4_CSI_CAMERA_SRC_HPP
