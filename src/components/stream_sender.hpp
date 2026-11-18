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
#include <memory>
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
#include "core/stream_telemetry.hpp"
#include "core/stream_queue_transport.hpp"
#include "core/thread_spawn.hpp"

namespace vstreamer
{

/*
 * Pad 0 (sink): STREAM_DGRAM or RTP in → UDP or queue egress (payload is opaque).
 *
 * Each SDU goes out as one media datagram, [stream_media_header][SDU], in input order. There is
 * no FEC here: the link protects the stream (winject-l3 runs FEC per upstream). Link reports from
 * the receiver come back on the reverse path and are kept for queries.
 */
class stream_sender : public component_sink
{
public:
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

    using component_sink::configure;
    int configure(std::string_view key, std::string_view value) override;
    using component_sink::query;
    int query(std::string_view key, std::string *value) const override;
    int query(uint64_t key, uint64_t *value) const override;
    int query(uint64_t key, int64_t *value) const override;
    int query(uint64_t key, double *value) const override;

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
    [[nodiscard]] bool wire_fits(size_t len);
    void evict_for_room();
    /* Stamps the next sequence number into buf's header and queues it. */
    void queue_wire(bfc::shared_sized_buffer &&buf);
    [[nodiscard]] int ingest_app_sdu(const uint8_t *data, size_t len, bool pdu_no_evict);

    static const std::vector<port_desc> &input_ports();

    [[nodiscard]] static size_t max_input_bytes();

    mutable std::mutex mu;
    bool               opened = false;

    std::string udp_transport_;
    std::string queue_transport_tx_;
    std::string queue_transport_rx_;
    std::shared_ptr<stream_transport_queue> transport_tx_;
    std::shared_ptr<stream_transport_queue> transport_rx_;

    int         send_fd = -1;
    sockaddr_in dst_addr {};
    bool        have_dst = false;

    buffer_pool pool;

    mutable std::mutex      q_mu;
    std::condition_variable q_cv;
    /* A wire datagram and when it was queued. */
    struct queued_wire
    {
        bfc::shared_sized_buffer              buf;
        std::chrono::steady_clock::time_point at {};
    };
    /* Wire datagrams in send order. */
    std::deque<queued_wire> queue;
    size_t                  queue_bytes = 0;
    /* Next media sequence number; stamped under q_mu, so wire order is sequence order. Starts
     * at a random value: a restarted sender does not replay recent numbers. */
    uint16_t                tx_seq = 0;
    /* Set by open(), cleared by close(): input() checks it without taking mu. */
    std::atomic<bool>       accepting {false};
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
    /* App SDUs queued for sending. */
    uint64_t           sdu_pkts_sent = 0;
    uint64_t           sdu_bytes_sent = 0;
    std::atomic<uint64_t> pkts_sent {0};
    std::atomic<uint64_t> bytes_sent {0};
    std::atomic<uint64_t> dropped {0};
    /* SDUs larger than max_input (also counted in dropped). */
    std::atomic<uint64_t> oversized {0};
    std::atomic<uint64_t> queue_full_rejects {0};
    /* Send queue: packets evicted to make room (also counted in dropped). */
    std::atomic<uint64_t> queue_evicted {0};
    /* Queue transport: packets the full TX queue evicted (still counted in pkts_sent). */
    std::atomic<uint64_t> transport_dropped {0};
    /* Largest time a datagram waited in the send queue since the last query (us). */
    mutable std::atomic<int64_t> send_late_max_us {0};
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_STREAM_SENDER_HPP
