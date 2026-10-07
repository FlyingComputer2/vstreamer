#ifndef VSTREAMER_COMPONENTS_H264_DECODER_MPP_HPP
#define VSTREAMER_COMPONENTS_H264_DECODER_MPP_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_H264_DECODER_MPP
#error "h264_decoder_mpp requires -DENABLE_H264_DECODER_MPP=ON"
#endif

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/buffer_pool.hpp"
#include "core/component_coder.hpp"
#include "core/component_pdu.hpp"
#include "core/output_opts.hpp"
#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"

#include <memory>

namespace vstreamer
{

class h264_decoder_mpp : public component_coder
{
public:
    h264_decoder_mpp();
    ~h264_decoder_mpp() override;

    h264_decoder_mpp(const h264_decoder_mpp &) = delete;
    h264_decoder_mpp &operator=(const h264_decoder_mpp &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;
    void cancel_pending_io();
    int input(component_pdu &&in) override;
    int output(component_pdu &out) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    [[nodiscard]] int input_pdu_locked(component_pdu &&in);
    [[nodiscard]] bool coded_caps_acceptable(const video_coded_caps &caps) const;
    void maybe_emit_output_caps_locked(int32_t w, int32_t h, int32_t hor, int32_t ver, uint64_t ts_us);
    void drain_thread_main();

    static const std::vector<port_desc> &input_ports();
    static const std::vector<port_desc> &output_ports();

    int  ensure_decoder_locked();
    void free_decoder_locked();
    void clear_pending_locked();
    int  handle_info_change_locked(void *mpp_frame);
    void drain_mpp_to_ready(int timeout_ms);
    int  fetch_one_mpp_frame(int timeout_ms);
    int  pack_mpp_to_ready_locked(void *mpp_frame);

    mutable std::mutex mu;
    std::mutex         mpp_io_mu;
    bool               opened = false;
    std::atomic<bool>  cancel_io {false};

    int width = 1280;
    int height = 720;
    int fps = 30;

    output_mode_e output_mode = output_mode_e::filter;
    sdu_type_e    output_format = sdu_type_e::NV12;

    void *ctx = nullptr;
    void *mpi = nullptr;
    void *frm_grp = nullptr;

    static constexpr size_t k_max_ready_frames = 8;
    bool                      output_size_stream = false;
    std::deque<component_pdu> ready_frames;
    std::deque<component_pdu> pending_caps_out_;

    bool             have_input_caps_ = false;
    bool             caps_reject_ = false;
    video_coded_caps input_caps_ {};
    bool             have_output_caps_ = false;
    video_raw_caps   output_caps_ {};
    uint64_t         out_seq_ = 0;

    std::unique_ptr<buffer_pool> nv12_pool;
    size_t                       nv12_pool_bytes = 0;

    double last_latency_ms = 0.0;

    std::thread       drain_thread_;
    std::atomic<bool> drain_stop_ {false};

    unsigned log_errinfo_throttle = 0;
    unsigned log_fbc_throttle = 0;
    unsigned log_pix_throttle = 0;
};

}  // namespace vstreamer

#endif
