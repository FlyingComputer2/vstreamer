#ifndef VSTREAMER_COMPONENTS_STREAM_SENDER_HPP
#define VSTREAMER_COMPONENTS_STREAM_SENDER_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_STREAM_SENDER
#error "stream_sender requires -DENABLE_STREAM_SENDER=ON"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <netinet/in.h>

#include "core/component_sink.hpp"
#include "core/data_packet.hpp"
#include "core/buffer_pool.hpp"
#include "core/rs_block_erasure.hpp"
#include "core/stream_telemetry.hpp"

namespace vstreamer
{

enum class fec_mode_e
{
    none,
    block,
};

/* Pad 0 (sink): SOCK in → UDP egress. */
class stream_sender : public component_sink
{
public:
    stream_sender();
    ~stream_sender() override;

    stream_sender(const stream_sender &) = delete;
    stream_sender &operator=(const stream_sender &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;

    [[nodiscard]] uint8_t input_pad_count() const override { return 1; }

    [[nodiscard]] packet_kind_e input_packet_kind(uint8_t port) const override;

    int  open() override;
    void close() override;

    int input(uint8_t port, const data_packet &in) override;

    int set_enabled(bool on, int timeout_ms) override;
    [[nodiscard]] bool enabled() const override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

    [[nodiscard]] stream_peer_link peer_link_snapshot() const;

    [[nodiscard]] const std::atomic<uint64_t> &wire_pkts_sent_counter() const
    {
        return pkts_sent;
    }
    [[nodiscard]] const std::atomic<uint64_t> &wire_bytes_sent_counter() const
    {
        return bytes_sent;
    }

private:
    void send_thread_main();
    void stop_send_thread();
    void stop_telemetry_thread();
    void telemetry_thread_main();
    void handle_link_report(const stream_link_report &report);
    void pace_wire_send(size_t bytes);
    void enqueue_wire_copy(const uint8_t *data, size_t len, bool is_fec_shard,
                           std::chrono::steady_clock::time_point release);
    void enqueue_fec_air(std::vector<std::vector<uint8_t>> *air);

    [[nodiscard]] size_t queue_byte_limit() const;

    [[nodiscard]] int effective_fec_k() const;
    [[nodiscard]] int effective_fec_n() const;
    [[nodiscard]] size_t max_fec_shard_bytes() const;
    [[nodiscard]] size_t max_raw_sdu_bytes() const;
    /* Flush partial block, (re)init with current k/n/timeout. No locks held. */
    int  reinit_fec_if_active();
    /* Flush partial block and stop encoding. No locks held. */
    void disable_fec();

    mutable std::mutex mu;
    bool               opened = false;

    std::string stream_spec;
    std::string local_spec = "0.0.0.0:0";
    bool        telemetry_on = true;
    int         mtu = 1400;
    int         max_datagram = 1472;

    int         send_fd = -1;
    sockaddr_in dst_addr {};
    bool        have_dst = false;

    bool              send_enabled = false;
    double            deadline_sec = 0;
    mutable std::mutex gate_mu;

    buffer_pool pool;

    /* A wire datagram and the earliest time the send thread may send it. */
    struct queued_wire
    {
        data_packet                           pkt;
        std::chrono::steady_clock::time_point release {};
    };

    mutable std::mutex      q_mu;
    std::condition_variable q_cv;
    /* Ordered by release time (ties keep enqueue order). */
    std::deque<queued_wire> queue;
    size_t                  queue_bytes = 0;
    /* Set (under q_mu) when input() opens a new FEC block: its timeout deadline is earlier
     * than whatever the send thread is sleeping toward, so the thread must recompute it. */
    bool                    fec_deadline_changed = false;
    static constexpr size_t k_queue_packet_cap = 1024;
    static constexpr size_t k_queue_min_paced_bytes = 32 * 1024;
    /* App-byte budget for a 1080p keyframe at k=8 n=15; unpaced wire minimum scales by n/k. */
    static constexpr size_t k_queue_min_unpaced_app_bytes = 136 * 1024;

    std::thread       send_thread;
    std::atomic<bool> send_stop {false};
    std::thread       telemetry_thread;
    std::atomic<bool> telemetry_stop {false};

    mutable std::mutex peer_mu;
    bool               peer_have = false;
    stream_link_report peer_report {};
    std::chrono::steady_clock::time_point peer_report_at {};
    uint32_t           peer_session = 0;
    uint16_t           peer_last_seq = 0;
    uint64_t           peer_last_timestamp_us = 0;
    bool               peer_have_timestamp = false;
    int64_t            peer_observed_interval_ms = -1;
    uint64_t           peer_reports_received = 0;
    uint64_t           peer_reports_lost = 0;
    uint64_t           peer_reports_rejected = 0;

    std::atomic<uint64_t> pkts_sent {0};
    std::atomic<uint64_t> bytes_sent {0};
    std::atomic<uint64_t> dropped {0};

    double   ingress_rate_t0 = 0.;
    uint64_t ingress_rate_bytes = 0;
    float    ingress_kbps = 0.f;

    std::atomic<int>  max_wire_kbps {0};
    std::atomic<bool> pace_reset {false};
    double            pace_bucket_bytes = 0.;
    double            pace_last_sec = 0.;
    int               queue_ms = 100;
    /* FEC block spreading: a block's shards are released evenly over this window instead of
     * back to back; 0 sends them at once. Blocks overlap, so their shards interleave on the
     * wire. Kept well under the receiver's head-of-line give-up (emit_hold_ms, 60 ms). */
    std::atomic<int>     fec_spread_ms {0};
    static constexpr int k_fec_spread_ms_max = 40;

    /* Guards fec (touched by input(), the send thread and configure()).
     * Lock order: mu -> fec_mu; never hold fec_mu while taking mu/q_mu. */
    mutable std::mutex fec_mu;
    rs_block_erasure   fec;
    fec_mode_e        fec_mode = fec_mode_e::block;
    int               fec_k = 10;
    int               fec_n = 12;
    int               fec_timeout_ms = rs_block_erasure::k_default_timeout_ms;
    uint64_t          fec_oversized = 0;

    std::atomic<uint16_t> stream_sequence {0};
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_STREAM_SENDER_HPP
