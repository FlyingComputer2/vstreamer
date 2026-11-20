#ifndef VSTREAMER_COMPONENTS_JPEG_ENCODER_ESP32P4_HPP
#define VSTREAMER_COMPONENTS_JPEG_ENCODER_ESP32P4_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_JPEG_ENCODER_ESP32P4
#error "jpeg_encoder_esp32p4 requires ENABLE_JPEG_ENCODER_ESP32P4 (ESP-IDF, esp32p4 target)"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/component_coder.hpp"
#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "core/thread_spawn.hpp"

namespace vstreamer
{

/*
 * O_UYY_E_VYY (ESP32-P4 ISP layout) -> MJPEG on the P4's hardware JPEG encoder (esp_driver_jpeg,
 * YUV420 input: chip revision 3.0 and later). Each frame is one keyframe.
 *
 * Input: CAPS_VIDEO_RAW, then frames. The encode runs on a worker of its own ("encode_cpu" /
 * "encode_prio"), one frame at a time: input() returns -EAGAIN while a frame is being encoded,
 * so a producer handing over camera buffers drops the frame rather than waiting on it. The
 * frame's buffer is released as soon as the engine has read it.
 *
 * Output: CAPS_VIDEO_CODED, then MJPEG frames in exact-size PSRAM buffers (the engine writes a
 * scratch buffer of "outbuf_kb"; a frame that does not fit is dropped and counted). Up to four
 * frames wait for output(); the oldest goes when a fifth is ready (out_drops).
 *
 * Keys: quality 1..100 (live, default 80); max_fps 0..240, frames closer than 1/max_fps to the
 * last one encoded are taken and skipped (live, 0 = every frame; rate_skips); outbuf_kb (while
 * closed; 0 = width*height/2 bytes, sized at the first caps).
 */
class jpeg_encoder_esp32p4 : public component_coder
{
public:
    jpeg_encoder_esp32p4();
    ~jpeg_encoder_esp32p4() override;
    jpeg_encoder_esp32p4(const jpeg_encoder_esp32p4 &) = delete;
    jpeg_encoder_esp32p4 &operator=(const jpeg_encoder_esp32p4 &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;
    int  input(component_pdu &&in) override;
    int  output(component_pdu &out) override;
    using component::configure;
    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    void worker_main();
    int  encode_one(component_pdu &&in, int w, int h, int quality);
    void push_out_locked(component_pdu &&out);

    /* hw_mu_ owns the engine and the scratch buffer across an encode; mu guards everything
     * else and is never held across hardware work. Order: hw_mu_, then mu. */
    std::mutex              hw_mu_;
    mutable std::mutex      mu;
    std::condition_variable cv_;
    bool                    opened_ = false;
    bool                    stop_ = false;

    void    *engine_ = nullptr; /* jpeg_encoder_handle_t */
    uint8_t *scratch_ = nullptr;
    size_t   scratch_cap_ = 0;

    int quality_ = 80;
    int max_fps_ = 0;
    int outbuf_kb_ = 0;

    bool             have_caps_ = false;
    video_raw_caps   caps_ {};
    bool             have_pending_ = false; /* worker's next frame */
    component_pdu    pending_;
    bool             busy_ = false;         /* pending taken, encode running */
    int64_t          last_ts_us_ = -1;      /* last frame accepted for encode */
    std::deque<component_pdu> out_q_;
    uint64_t         out_seq_ = 0;

    uint64_t frames_ = 0, busy_drops_ = 0, rate_skips_ = 0, errors_ = 0, overflows_ = 0;
    uint64_t out_drops_ = 0, bytes_ = 0;
    int64_t  enc_us_sum_ = 0, enc_us_max_ = 0;
    int      last_error_ = 0;

    std::thread  worker_;
    thread_place encode_place_;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_JPEG_ENCODER_ESP32P4_HPP
