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

/* lwIP declares sockaddr_in in <sys/socket.h>, not <netinet/in.h>. */
#include <sys/socket.h>
#include <netinet/in.h>

#include "core/component_sink.hpp"
#include "core/buffer_pool.hpp"
#include "core/component_input.hpp"
#include "core/default_reactor.hpp"
#include "core/port_caps.hpp"
#include "core/rs_block_erasure.hpp"
#include "core/stream_telemetry.hpp"
#include "core/stream_queue_transport.hpp"
#include "core/thread_spawn.hpp"

namespace vstreamer
{

enum class fec_mode_e
{
    none,
    block,
};

/* Pad 0 (sink): STREAM_DGRAM or RTP in → UDP or queue egress (payload is opaque). */
class stream_sender : public component_sink
{
public:
    /* Largest fec_spread_ms: the receiver assumes this spread until it has measured one. */
    static constexpr int k_fec_spread_max_ms = rs_block_erasure::k_spread_cap_ms;
    /* Fixed UDP payload ceiling (winject MPDU path). Pay/encoder must packetize within this. */
    static constexpr int k_wire_datagram_bytes = 1476;

    stream_sender();
    ~stream_sender() override;

    stream_sender(const stream_sender &) = delete;
    stream_sender &operator=(const stream_sender &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;
    int input(component_pdu &&in) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    [[nodiscard]] stream_peer_link peer_link_snapshot() const;
    struct send_thread_wdt;
    void send_thread_main();
    void stop_send_thread();
    void stop_reactor();
    void on_queue_report_read();
    void ingest_link_report(const uint8_t *data, size_t len);
    void on_telemetry_socket_read();
    void handle_link_report(const stream_link_report &report);
    void enqueue_wire_copy(const uint8_t *data, size_t len, bool is_fec_shard,
                           std::chrono::steady_clock::time_point release);
    /* Enqueues without evicting; -EAGAIN when the queue is full, unless force is set. */
    [[nodiscard]] int try_enqueue_wire_copy(const uint8_t *data, size_t len, bool is_fec_shard,
                                            std::chrono::steady_clock::time_point release,
                                            bool force = false);
    /* True when the queue has room for every wire packet one app SDU can produce. */
    [[nodiscard]] bool queue_has_room_for_sdu() const;
    void enqueue_fec_air(std::vector<std::vector<uint8_t>> *air);

    [[nodiscard]] int ingest_app_sdu(const uint8_t *data, size_t len, bool pdu_no_evict);

    static const std::vector<port_desc> &input_ports();

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

    std::string udp_transport_;
    std::string queue_transport_tx_;
    std::string queue_transport_rx_;
    std::shared_ptr<stream_transport_queue> transport_tx_;
    std::shared_ptr<stream_transport_queue> transport_rx_;
    bool        telemetry_on = true;

    int         send_fd = -1;
    sockaddr_in dst_addr {};
    bool        have_dst = false;

    buffer_pool pool;

    /* A wire datagram and the earliest time the send thread may send it. */
    struct queued_wire
    {
        bfc::shared_sized_buffer                   buf;
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
    /* Fixed send-queue capacity in packets (= buffer pool size); input() gets -EAGAIN past it. */
    static constexpr size_t k_queue_packet_cap = 1024;

    std::thread       send_thread;
    thread_place      send_place;      /* keys send_cpu / send_prio */
    std::atomic<bool> send_stop {false};
    std::string reactor_name_;
    std::shared_ptr<default_reactor> udp_reactor_;
    std::shared_ptr<bfc::cv_reactor<>> queue_reactor_;
    bool               reactor_running_ = false;
    bool               rx_registered_ = false; /* report handler is on queue_reactor_ */

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

    /* Local tuple fields are updated and sampled under metrics_mu. */
    mutable std::mutex metrics_mu;
    uint64_t           fec_pkts_sent = 0;
    uint64_t           fec_bytes_sent = 0;
    std::atomic<uint64_t> pkts_sent {0};
    std::atomic<uint64_t> bytes_sent {0};
    std::atomic<uint64_t> dropped {0};
    std::atomic<uint64_t> queue_full_rejects {0};
    /* Send queue: packets evicted to make room (also counted in dropped). */
    std::atomic<uint64_t> queue_evicted {0};
    /* Queue transport: packets the full TX queue evicted (still counted in pkts_sent). */
    std::atomic<uint64_t> transport_dropped {0};
    std::atomic<uint64_t> fec_push_us {0}; /* time in fec.push_app (RS encode included) */

    /* FEC block spreading: a block's shards are released evenly over this window instead of
     * back to back; 0 sends them at once. Blocks overlap, so their shards interleave on the wire.
     * Capped at k_fec_spread_max_ms. Shards wait in the send queue until due, so about
     * rate x spread worth of packets are queued against k_queue_packet_cap. */
    std::atomic<int> fec_spread_ms {0};
    /* Largest delay of a datagram past its release time since the last query (us). */
    mutable std::atomic<int64_t> send_late_max_us {0};

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
