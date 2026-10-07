#ifndef VSTREAMER_COMPONENTS_H264_ENCODER_MPP_HPP
#define VSTREAMER_COMPONENTS_H264_ENCODER_MPP_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_H264_ENCODER_MPP
#error "h264_encoder_mpp requires -DENABLE_H264_ENCODER_MPP=ON"
#endif

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#include <string>
#include <string_view>

#include "core/component_coder.hpp"
#include "core/component_pdu.hpp"
#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"

namespace vstreamer
{

/* Packed NV12 → H.264 Annex-B (Rockchip MPP, RK3588 / OPI5). */
class h264_encoder_mpp : public component_coder
{
public:
    h264_encoder_mpp();
    ~h264_encoder_mpp() override;

    h264_encoder_mpp(const h264_encoder_mpp &) = delete;
    h264_encoder_mpp &operator=(const h264_encoder_mpp &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;

    /* Wake blocking encode drain / output waits (shutdown without joining a stuck thread). */
    void cancel_pending_io();
    int input(component_pdu &&in) override;
    int output(component_pdu &out) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    int  encoder_open_locked();
    void encoder_close_locked();
    int  reopen_if_needed_locked();
    int  apply_h264_cfg_locked();
    int  apply_rc_cfg_locked();
    int  drain_packets_locked(int timeout_ms);
    int  put_nv12_frame_unlocked(const component_pdu &in);
    void clear_out_locked();
    void release_enc_slot_after_eoi();
    bool append_enc_packet_bytes(const uint8_t *data, size_t len);
    bool finalize_enc_au_pdu(component_pdu *out);
    void enqueue_completed_aus_locked(std::vector<component_pdu> &&aus);
    /* mpp_api_mu must already be held; never takes mu. */
    void ingest_enc_packet(void *mpp_packet_opaque, std::vector<component_pdu> *completed_aus);
    void drain_enc_packets_nonblock(void *mpp_ctx, void *mpp_mpi,
                                    std::vector<component_pdu> *completed_aus);

    /* Lock order: mu → mpp_api_mu only; never take mu while holding mpp_api_mu. */
    mutable std::mutex      mu;
    std::mutex              mpp_api_mu;
    std::condition_variable cv;

    bool opened = false;
    bool reopen_req = false;
    std::atomic<bool> cancel_io {false};

    int width = 1920;
    int height = 1080;
    int fps = 30;
    int qp = 36;
    int gop = 30;
    /* Target wire bitrate (bit/s); metrics h264_encoder.cbr_kbps. Honored by MPP when rc_mpp_cbr. */
    int  bps_target = 20'000'000;
    bool rc_mpp_cbr = false;
    double super_i_ratio = 6.0;
    double super_p_ratio = 1.5;
    bool   pending_idr = false;

    int live_w = 0;
    int live_h = 0;
    int live_hor = 0;
    int live_ver = 0;
    int live_fps = 0;
    int live_qp = 0;
    int live_gop = 0;
    int live_bps = 0;

    /* Opaque MPP handles; typed in the .cpp. */
    void *ctx = nullptr;
    void *mpi = nullptr;
    void *enc_cfg = nullptr;
    void *frm_grp = nullptr;
    void *md_info = nullptr;
    uint64_t enc_frames_in = 0;

    std::vector<uint8_t> enc_au_accum;
    int64_t              enc_au_pts = 0;
    int64_t              enc_au_mono_ns = 0;
    bool                 enc_au_key = false;

    static constexpr int enc_slot_count = 8;
    struct enc_slot
    {
        void   *frm = nullptr;
        void   *pkt = nullptr;
        int64_t frame_mono_ns = 0;
    };
    std::array<enc_slot, enc_slot_count> enc_slots {};
    std::deque<int>                      enc_free_slots;
    std::deque<int>                      enc_pending_slots;

    std::deque<component_pdu> out_q;

    bool             have_input_caps_ = false;
    bool             caps_reject_ = false;
    video_raw_caps   input_caps_ {};
    bool             have_output_caps_ = false;
    video_coded_caps output_caps_ {};
    uint64_t         out_seq_ = 0;
    std::deque<component_pdu> pending_caps_out_;

    double last_latency_ms = 0.0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_H264_ENCODER_MPP_HPP
