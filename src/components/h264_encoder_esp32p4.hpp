#ifndef VSTREAMER_COMPONENTS_H264_ENCODER_ESP32P4_HPP
#define VSTREAMER_COMPONENTS_H264_ENCODER_ESP32P4_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_H264_ENCODER_ESP32P4
#error "h264_encoder_esp32p4 requires ENABLE_H264_ENCODER_ESP32P4 (ESP-IDF, esp32p4 target)"
#endif

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "core/component_coder.hpp"
#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"

namespace vstreamer
{

/*
 * O_UYY_E_VYY (ESP32-P4 ISP layout) → H.264 on the P4's hardware encoder (esp_h264).
 * Encoding is synchronous in input(): about 35 ms per 1080p frame on the calling thread.
 */
class h264_encoder_esp32p4 : public component_coder
{
public:
    h264_encoder_esp32p4();
    ~h264_encoder_esp32p4() override;

    h264_encoder_esp32p4(const h264_encoder_esp32p4 &) = delete;
    h264_encoder_esp32p4 &operator=(const h264_encoder_esp32p4 &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;
    int input(component_pdu &&in) override;
    int output(component_pdu &out) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    int  codec_open_locked();
    void codec_close_locked();
    int  input_caps_locked(const component_pdu &in);
    int  encode_locked(const component_pdu &in);

    mutable std::mutex mu;

    bool opened = false;
    bool reopen_req = false;

    int      width = 1920;
    int      height = 1080;
    int      fps = 30;
    int      gop = 30;
    uint32_t bitrate = 4000000;
    int      qp_min = 20;
    int      qp_max = 40;
    bool     pending_idr = false;

    /* esp_h264 handles and the cache-aligned bitstream buffer; typed in the .cpp. */
    void    *enc_ = nullptr;
    void    *param_ = nullptr;
    uint8_t *out_buf_ = nullptr;
    uint32_t out_cap_ = 0;
    int      live_w_ = 0;
    int      live_h_ = 0;

    bool             have_input_caps_ = false;
    bool             caps_reject_ = false;
    video_coded_caps caps_out_ {}; /* last CAPS_VIDEO_CODED sent */
    std::deque<component_pdu> out_q_;
    uint64_t         out_seq_ = 0;

    uint64_t frames_ = 0;
    uint64_t idr_frames_ = 0;
    uint64_t encode_errors_ = 0;
    int64_t  last_encode_us_ = 0;
    int64_t  max_encode_us_ = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_H264_ENCODER_ESP32P4_HPP
