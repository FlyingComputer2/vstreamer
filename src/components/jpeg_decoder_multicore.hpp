#ifndef VSTREAMER_COMPONENTS_JPEG_DECODER_MULTICORE_HPP
#define VSTREAMER_COMPONENTS_JPEG_DECODER_MULTICORE_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_JPEG_DECODER_MULTICORE
#error "jpeg_decoder_multicore requires -DENABLE_JPEG_DECODER_MULTICORE=ON"
#endif

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/buffer_pool.hpp"
#include "core/component_coder.hpp"
#include "core/component_pdu.hpp"
#include "core/output_opts.hpp"
#include "core/pdu_input.hpp"
#include "core/pdu_output.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"

#include <memory>

namespace vstreamer
{

class jpeg_decoder_multicore : public component_coder, public pdu_input, public pdu_output
{
public:
    jpeg_decoder_multicore();
    ~jpeg_decoder_multicore() override;

    jpeg_decoder_multicore(const jpeg_decoder_multicore &) = delete;
    jpeg_decoder_multicore &operator=(const jpeg_decoder_multicore &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    int input(uint8_t port, const data_packet &in) override;
    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int input(component_pdu &&in) override;
    int output(component_pdu &out) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    [[nodiscard]] int input_pdu_locked(component_pdu &&in);
    [[nodiscard]] bool caps_acceptable(const video_coded_caps &caps) const;
    void maybe_emit_output_caps_locked(uint64_t ts_us);

    static const std::vector<port_desc> &input_ports();
    static const std::vector<port_desc> &output_ports();

    static constexpr int k_max_workers = 8;
    static constexpr int k_queue_depth = 32;
    static constexpr size_t k_max_jpeg = 8ULL * 1024ULL * 1024ULL;

    struct job
    {
        uint64_t seq = 0;
        uint8_t *data = nullptr;
        size_t   size = 0;
        uint64_t ts_us = 0;
    };

    struct result_slot
    {
        uint64_t      seq = 0;
        int           status = 0;
        bool          ready = false;
        component_pdu pdu;
    };

    void worker_main(int worker_index);
    int  decode_one(void *dec, void *avframe, void *pkt, const job &j, component_pdu *out);
    int  start_workers();
    void stop_workers();

    mutable std::mutex cfg_mu;
    int                width = 1280;
    int                height = 720;
    int                fps = 30;
    int                workers = 2;
    int                worker_cpu = -1;
    std::vector<int>   worker_cpus;
    output_mode_e      output_mode = output_mode_e::filter;
    media_kind_e       output_format = media_kind_e::NV12;
    mutable std::string decoded_pix_fmt = "unknown";

    mutable std::mutex       life_mu;
    bool                     opened = false;
    bool                     stop = false;
    std::vector<std::thread> threads;

    std::mutex              job_mu;
    std::condition_variable job_cv;
    job                     jobs[k_queue_depth];
    int                     job_head = 0;
    int                     job_tail = 0;
    int                     job_count = 0;
    uint64_t                next_in_seq = 0;

    std::mutex              res_mu;
    std::condition_variable res_cv;
    result_slot             results[k_queue_depth];
    uint64_t                next_out_seq = 0;

    mutable bool unsupported_pix_fmt_log_done = false;

    mutable std::mutex                   nv12_pool_mu;
    mutable std::unique_ptr<buffer_pool> nv12_pool;
    mutable size_t                       nv12_pool_bytes = 0;

    bool                      have_input_caps_ = false;
    video_coded_caps          input_caps_ {};
    bool                      caps_reject_ = false;
    bool                      have_output_caps_ = false;
    video_raw_caps            output_caps_ {};
    uint64_t                  out_seq_ = 0;
    std::deque<component_pdu> pending_caps_out_;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_JPEG_DECODER_MULTICORE_HPP
