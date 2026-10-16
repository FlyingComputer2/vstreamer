#ifndef VSTREAMER_COMPONENTS_ESP32P4_CSI_CAMERA_SRC_HPP
#define VSTREAMER_COMPONENTS_ESP32P4_CSI_CAMERA_SRC_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_ESP32P4_CSI_CAMERA_SRC
#error "esp32p4_csi_camera_src requires ENABLE_ESP32P4_CSI_CAMERA_SRC (ESP-IDF, esp32p4 target)"
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "core/component_pdu.hpp"
#include "core/component_source.hpp"

namespace vstreamer
{

/*
 * MIPI-CSI camera → ISP → O_UYY_E_VYY on the ESP32-P4 (esp_video, /dev/video0). Frames are
 * the driver's own buffers, not copies: each one goes back to the driver when the last
 * reference to its PDU drops, so a consumer that holds frames stalls capture. A capture thread
 * keeps only the newest frame; output() hands it out after CAPS_VIDEO_RAW.
 *
 * esp_video_init() runs once per boot on the first open(); the sccb_* / *_pin keys only
 * matter before that. The sensor and its mode come from sdkconfig (esp_cam_sensor).
 */
class esp32p4_csi_camera_src : public component_source
{
public:
    esp32p4_csi_camera_src();
    ~esp32p4_csi_camera_src() override;

    esp32p4_csi_camera_src(const esp32p4_csi_camera_src &) = delete;
    esp32p4_csi_camera_src &operator=(const esp32p4_csi_camera_src &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;
    int  output(component_pdu &out) override;
    /* Blocks up to timeout_ms for a frame (or CAPS once). Returns -EAGAIN on timeout. */
    int  wait_output(component_pdu &out, int timeout_ms);

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

    /* Driver buffers, shared with the release callbacks of frames still downstream. */
    struct ring;

private:
    int  capture_open_locked();
    void capture_main();
    /* Caller holds mu. 0 = delivered, -EAGAIN = nothing ready. */
    int  try_pop_output_locked(component_pdu &out);
    /* STREAMOFF, wait for downstream frames, then unmap and close. */
    static void release_ring(std::shared_ptr<ring> r);

    mutable std::mutex              mu;
    std::condition_variable         out_cv_;

    /* esp_video_init() parameters. */
    int sccb_port = 0;
    int sccb_scl = 8;
    int sccb_sda = 7;
    int sccb_freq = 100000;
    int reset_pin = -1;
    int pwdn_pin = -1;

    int width = 0;  /* 0 = the sensor's mode */
    int height = 0;
    int fps = 30;   /* for the caps; the sensor mode sets the real rate */
    int nbufs = 4;

    bool        source_open = false;
    std::string driver;
    int         live_w = 0;
    int         live_h = 0;

    std::shared_ptr<ring>        ring_;
    std::thread                  thread_;
    std::atomic<bool>            stop_ {false};
    std::optional<component_pdu> ready_;
    bool                         caps_sent_ = false;
    uint64_t                     out_seq_ = 0;

    uint64_t frames_ = 0;
    uint64_t dropped_ = 0;     /* replaced in ready_ before output() took them */
    uint64_t dqbuf_errors_ = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_ESP32P4_CSI_CAMERA_SRC_HPP
