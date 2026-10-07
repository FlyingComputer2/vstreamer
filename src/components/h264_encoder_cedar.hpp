#ifndef VSTREAMER_COMPONENTS_H264_ENCODER_CEDAR_HPP
#define VSTREAMER_COMPONENTS_H264_ENCODER_CEDAR_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_H264_ENCODER_CEDAR
#error "h264_encoder_cedar requires -DENABLE_H264_ENCODER_CEDAR=ON"
#endif

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "core/component_coder.hpp"
#include "core/component_pdu.hpp"
#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/sdu_caps.hpp"

namespace vstreamer
{

/* NV12 → H.264 via libavcodec h264_cedrus (/dev/cedar_dev). */
class h264_encoder_cedar : public component_coder
{
public:
    h264_encoder_cedar();
    ~h264_encoder_cedar() override;

    h264_encoder_cedar(const h264_encoder_cedar &) = delete;
    h264_encoder_cedar &operator=(const h264_encoder_cedar &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;
    int input(component_pdu &&in) override;
    int output(component_pdu &out) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    [[nodiscard]] int nv12_size_locked() const;
    int  codec_open_locked();
    void codec_close_locked();
    int  reopen_if_needed_locked();
    int  drain_packets_locked();
    void clear_out_locked();

    mutable std::mutex      mu;
    std::condition_variable cv;

    bool opened = false;
    bool reopen_req = false;

    int width = 1280;
    int height = 720;
    int fps = 30;
    int qp = 36;
    int gop = 30;

    /* Opaque libav handles; typed in the .cpp. */
    void *ctx = nullptr;
    void *avframe = nullptr;
    void *pkt = nullptr;
    int   live_w = 0;
    int   live_h = 0;
    int   live_fps = 0;
    int   live_qp = 0;
    int   live_gop = 0;
    bool  pending_idr = false;

    std::deque<uint64_t> in_capture_ts_us_;

    std::deque<component_pdu> out_q;

    bool             have_input_caps_ = false;
    bool             caps_reject_ = false;
    video_raw_caps   input_caps_ {};
    bool             have_output_caps_ = false;
    video_coded_caps output_caps_ {};
    uint64_t         out_seq_ = 0;
    std::deque<component_pdu> pending_caps_out_;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_H264_ENCODER_CEDAR_HPP
