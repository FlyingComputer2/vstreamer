#ifndef VSTREAMER_COMPONENTS_ESP32_H264_ISP_SRC_HPP
#define VSTREAMER_COMPONENTS_ESP32_H264_ISP_SRC_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_ESP32_H264_ISP_SRC
#error "esp32_h264_isp_src requires ENABLE_ESP32_H264_ISP_SRC (ESP-IDF, esp32p4)"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/component_pdu.hpp"
#include "core/component_source.hpp"
#include "core/port_caps.hpp"
#include "core/thread_spawn.hpp"

namespace vstreamer
{

class esp32p4_csi_camera_src;
class h264_encoder_esp32p4;

/*
 * MIPI-CSI / ISP capture (esp_video) fused with P4 hardware H.264 (esp_h264).
 * Outputs CAPS_VIDEO_CODED + H264_AU on one source port — no separate csi_camera +
 * h264_encoder nodes in the pipeline graph.
 *
 * YUV stays in CSI mmap buffers until encode (USERPTR-style); encode uses the same
 * esp_h264 path as h264_encoder_esp32p4. vs_isp_enc pulls frames from the camera
 * source directly (no intermediate raw queue).
 */
class esp32_h264_isp_src : public component_source
{
public:
    esp32_h264_isp_src();
    ~esp32_h264_isp_src() override;

    esp32_h264_isp_src(const esp32_h264_isp_src &) = delete;
    esp32_h264_isp_src &operator=(const esp32_h264_isp_src &) = delete;

    [[nodiscard]] std::string name() const override;
    int                       open() override;
    void                      close() override;
    int                       output(component_pdu &out) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    static const std::vector<port_desc> &output_ports();
    void encode_main();
    int  push_encoder_input(component_pdu &in);
    void drain_encoder_output();

    std::unique_ptr<esp32p4_csi_camera_src> cam_;
    std::unique_ptr<h264_encoder_esp32p4>     enc_;

    mutable std::mutex              mu;
    std::condition_variable           out_cv_;
    std::deque<component_pdu>         out_q_;
    static constexpr size_t           k_out_q_max = 8;
    /* A camera frame is encoded only while fewer coded AUs than this wait for output(); otherwise
     * it is dropped, so a slow consumer costs frames instead of queueing latency. */
    static constexpr size_t           k_coded_backlog_max = 1;
    uint64_t                          backlog_drops_ = 0;

    std::thread                       encode_thread_;
    thread_place                      encode_place_; /* keys encode_cpu / encode_prio */
    std::atomic<bool>                 stop_ {false};
    /* Standard latency queries: camera DQBUF to output() of the last AU. The node's entry is the
     * capture, so node_latency_ms is the same value. */
    std::atomic<double>               latency_ms_ {-1.0};
    bool                              opened = false;

    int      fps = 30;
    int      gop = 30;
    uint32_t bitrate = 4000000;
    int      qp_min = 20;
    int      qp_max = 51;
    int      buffers = 6;

    uint64_t wall_sum_us_ = 0;
    uint64_t wall_frames_ = 0;
    int64_t  last_wall_us_ = 0;
    uint64_t wait_sum_us_ = 0;
    uint64_t wait_samples_ = 0;
    uint64_t push_sum_us_ = 0;
    uint64_t push_samples_ = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_ESP32_H264_ISP_SRC_HPP
