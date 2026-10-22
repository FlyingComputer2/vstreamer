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
 * nonblocking. */
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
    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

   private:
    void encode_main();
    void drain_encoder();
    std::unique_ptr<detail::esp32p4_csi_device> camera_;
    std::unique_ptr<h264_encoder_esp32p4> encoder_;
    // Lifecycle lock prevents configure/open from racing a close that is
    // joining.
    std::mutex lifecycle_mu_;
    mutable std::mutex mu_;
    std::deque<component_pdu> out_q_;
    std::thread worker_;
    thread_place encode_place_;
    std::atomic<bool> stop_{false};
    std::atomic<double> latency_ms_{-1.0};
    bool opened_ = false;
    int last_error_ = 0;
    uint64_t frames_ = 0, backlog_drops_ = 0, dqbuf_errors_ = 0, encode_errors_ = 0;
};
}  // namespace vstreamer
#endif
