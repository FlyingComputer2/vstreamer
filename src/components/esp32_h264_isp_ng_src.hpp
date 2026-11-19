#ifndef VSTREAMER_COMPONENTS_ESP32_H264_ISP_NG_SRC_HPP
#define VSTREAMER_COMPONENTS_ESP32_H264_ISP_NG_SRC_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_ESP32_H264_ISP_NG_SRC
#error "esp32_h264_isp_ng_src requires ENABLE_ESP32_H264_ISP_NG_SRC (ESP-IDF, esp32p4)"
#endif
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "core/component_source.hpp"
#include "core/thread_spawn.hpp"

namespace vstreamer
{
namespace detail
{
class esp32p4_csi_device;
}
class h264_encoder_esp32p4;

/* CSI/ISP DMA buffers are the encoder's raw input buffers. One worker dequeues,
 * encodes synchronously, and returns each buffer to capture. No camera
 * component, raw-frame ready queue, or task-to-task frame handoff. output() is
 * nonblocking.
 *
 * Port 0: CAPS_VIDEO_CODED, then H264_AU. Port 1, while "raw_out" is 1 (live):
 * CAPS_VIDEO_RAW, then each camera frame (O_UYY_E_VYY) as it goes to the
 * encoder, zero-copy: the PDU holds the camera buffer until its last reference
 * drops, so a consumer must let go quickly (capture runs on "buffers" buffers).
 * One raw frame waits at most; a newer one replaces it (raw_drops). Port 0
 * comes out first. */
class esp32_h264_isp_ng_src : public component_source
{
   public:
    esp32_h264_isp_ng_src();
    ~esp32_h264_isp_ng_src() override;
    esp32_h264_isp_ng_src(const esp32_h264_isp_ng_src &) = delete;
    esp32_h264_isp_ng_src &operator=(const esp32_h264_isp_ng_src &) = delete;
    [[nodiscard]] std::string name() const override;
    int open() override;
    void close() override;
    int output(component_pdu &out) override;
    using component::configure;
    int configure(std::string_view key, std::string_view value) override;
    /* bitrate, gop and idr go straight to the encoder, which validates them; live while open. */
    int configure(uint64_t key, uint64_t value) override;
    int query(std::string_view key, std::string *value) const override;

   private:
    void encode_main();
    void drain_encoder();
    void offer_raw(const component_pdu &frame);
    std::unique_ptr<detail::esp32p4_csi_device> camera_;
    std::unique_ptr<h264_encoder_esp32p4> encoder_;
    // Lifecycle lock prevents configure/open from racing a close that is
    // joining.
    std::mutex lifecycle_mu_;
    mutable std::mutex mu_;
    std::deque<component_pdu> out_q_;
    /* Port 1 (mu_): the waiting raw frame, and caps to send before the next one. */
    std::atomic<bool> raw_out_{false};
    bool raw_caps_pending_ = false;
    bool have_raw_ = false;
    component_pdu raw_;
    std::thread worker_;
    thread_place encode_place_;
    std::atomic<bool> stop_{false};
    std::atomic<double> latency_ms_{-1.0};
    bool opened_ = false;
    int last_error_ = 0;
    uint64_t frames_ = 0, backlog_drops_ = 0, dqbuf_errors_ = 0, encode_errors_ = 0;
    uint64_t raw_frames_ = 0, raw_drops_ = 0;
};
}  // namespace vstreamer
#endif
