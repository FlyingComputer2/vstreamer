#ifndef VSTREAMER_TEST_APP_DIAG_HPP
#define VSTREAMER_TEST_APP_DIAG_HPP

#include "test_app/stream_sdl/encoder_types.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "components/components.hpp"

namespace vstreamer::test_app
{

class channel_controller;

struct bench_diag
{
    std::atomic<uint64_t> tx_noise {0};
    std::atomic<uint64_t> tx_source_bytes {0};
    std::atomic<uint64_t> tx_mjpeg_q_drop {0};
    std::atomic<uint64_t> tx_nv12_q_drop {0};
    std::atomic<uint64_t> tx_jpeg_nv12 {0};
    std::atomic<uint64_t> tx_jpeg_nv12_bytes {0};
    std::atomic<uint64_t> tx_enc_nv12_popped {0};
    std::atomic<uint64_t> tx_nv12 {0};
    std::atomic<uint64_t> tx_enc_input_miss {0};
    std::atomic<uint64_t> tx_enc_in_err {0};
    std::atomic<uint64_t> tx_enc_skip {0};
    std::atomic<uint64_t> tx_enc_out_bytes {0};
    std::atomic<uint64_t> tx_rtp_sock {0};
    std::atomic<uint64_t> rx_udp {0};
    std::atomic<uint64_t> rx_depay_err {0};
    std::atomic<uint64_t> rx_depay_au {0};
    std::atomic<uint64_t> rx_dec_in_ok {0};
    std::atomic<uint64_t> rx_dec_in_bytes {0};
    std::atomic<uint64_t> rx_dec_in_eagain {0};
    std::atomic<uint64_t> rx_dec_in_err {0};
    std::atomic<uint64_t> rx_dec_out_eagain {0};
    std::atomic<uint64_t> rx_dec_out_err {0};
    std::atomic<uint64_t> rx_nv12_out {0};
    std::atomic<uint64_t> rx_nv12_out_bytes {0};
    std::atomic<uint64_t> rx_present_ok {0};
    std::atomic<uint64_t> rx_present_err {0};
    std::atomic<uint64_t> rx_present_q_drop {0};
    std::atomic<uint64_t> rx_au_q_drop {0};
};
struct pipeline_counters
{
    uint64_t tx_noise = 0;
    uint64_t tx_source_bytes = 0;
    uint64_t tx_jpeg_nv12 = 0;
    uint64_t tx_jpeg_nv12_bytes = 0;
    uint64_t tx_mjpeg_q_drop = 0;
    uint64_t tx_nv12_q_drop = 0;
    uint64_t tx_enc_nv12_popped = 0;
    uint64_t tx_nv12 = 0;
    uint64_t tx_enc_input_miss = 0;
    uint64_t tx_enc_out_bytes = 0;
    uint64_t tx_rtp_sock = 0;
    uint64_t rx_udp = 0;
    uint64_t rx_dec_in_ok = 0;
    uint64_t rx_dec_in_bytes = 0;
    uint64_t rx_depay_au = 0;
    uint64_t rx_nv12_out = 0;
    uint64_t rx_nv12_out_bytes = 0;
    uint64_t rx_present_ok = 0;
};

struct pipeline_rate_state
{
    std::chrono::steady_clock::time_point t0 {};
    pipeline_counters                       snap {};
    uint64_t                                snd_pkts = 0;
    uint64_t                                snd_bytes = 0;
    uint64_t                                enc_out_bytes = 0;
    uint64_t                                dec_dropped = 0;
    uint64_t                                sink_dropped = 0;
    uint64_t                                rcv_bytes = 0;
    uint64_t                                ch_bytes_in = 0;
    uint64_t                                ch_bytes_out = 0;
    uint64_t                                ch_pkts_out = 0;
    uint64_t                                ch_fwd_drops = 0;
    bool                                    have_snap = false;
};

void log_bench_diag(const bench_diag &d, vstreamer::stream_receiver &rcv,
                    vstreamer::stream_sender &sender, h264_encoder_t &enc,
                    const channel_controller *channel);

void log_bench_rate_line(const pipeline_rate_state &rate, h264_encoder_t &enc,
                         const bench_diag &diag);

double query_component_latency_ms(vstreamer::component &c);

}  // namespace vstreamer::test_app

#endif
