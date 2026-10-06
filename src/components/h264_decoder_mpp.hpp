#ifndef VSTREAMER_COMPONENTS_H264_DECODER_MPP_HPP
#define VSTREAMER_COMPONENTS_H264_DECODER_MPP_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_H264_DECODER_MPP
#error "h264_decoder_mpp requires -DENABLE_H264_DECODER_MPP=ON"
#endif

#include <atomic>
#include <cstdint>
#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include "core/buffer_pool.hpp"
#include "core/component_coder.hpp"
#include "core/output_opts.hpp"

#include <memory>

namespace vstreamer
{

/* Rockchip MPP H.264 (Annex-B) → packed NV12 (GS receive path). */
class h264_decoder_mpp : public component_coder
{
public:
    h264_decoder_mpp();
    ~h264_decoder_mpp() override;

    h264_decoder_mpp(const h264_decoder_mpp &) = delete;
    h264_decoder_mpp &operator=(const h264_decoder_mpp &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    void cancel_pending_io();

    int input(uint8_t port, const data_packet &in) override;
    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    int  ensure_decoder_locked();
    void free_decoder_locked();
    void clear_pending_locked();
    int  handle_info_change_locked(void *mpp_frame);
    /* Poll MPP and pack frames; caller must not hold mu. */
    void drain_mpp_to_ready(int timeout_ms);
    int  fetch_one_mpp_frame(int timeout_ms);
    int  pack_mpp_to_ready_locked(void *mpp_frame);
    void remember_capture_pts(int64_t pts, int64_t capture_mono_ns, int64_t input_mono_ns);
    [[nodiscard]] int64_t lookup_input_mono_pts(int64_t pts) const;
    int64_t lookup_capture_pts(int64_t pts) const;

    /* Lock order: mu → mpp_io_mu only. */
    mutable std::mutex mu;
    std::mutex         mpp_io_mu;
    bool               opened = false;
    std::atomic<bool>  cancel_io {false};

    int width = 1280;
    int height = 720;
    int fps = 30;

    output_mode_e output_mode = output_mode_e::filter;
    media_kind_e  output_format = media_kind_e::NV12;

    /* Opaque MPP handles; typed in the .cpp. */
    void *ctx = nullptr;
    void *mpi = nullptr;
    void *frm_grp = nullptr;

    static constexpr size_t k_max_ready_frames = 8;
    static constexpr size_t k_pts_ring = 64;
    struct pts_capture_entry
    {
        int64_t pts = 0;
        int64_t capture_mono_ns = 0;
        int64_t input_mono_ns = 0;
    };
    std::array<pts_capture_entry, k_pts_ring> pts_ring {};
    size_t                                   pts_ring_head = 0;
    bool                                     output_size_stream = false;
    std::deque<frame>                        ready_frames;

    std::unique_ptr<buffer_pool> nv12_pool;
    size_t                       nv12_pool_bytes = 0;

    /* Capture-to-decoded-frame (ms); updated when output carries capture_mono_ns. */
    double last_latency_ms = 0.0;
    double last_node_latency_ms = 0.0;

    unsigned log_errinfo_throttle = 0;
    unsigned log_fbc_throttle = 0;
    unsigned log_pix_throttle = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_H264_DECODER_MPP_HPP
