#ifndef VSTREAMER_COMPONENTS_STREAM_RECEIVER_HPP
#define VSTREAMER_COMPONENTS_STREAM_RECEIVER_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_STREAM_RECEIVER
#error "stream_receiver requires -DENABLE_STREAM_RECEIVER=ON"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/component_source.hpp"
#include "core/buffer_pool.hpp"
#include "core/component_output.hpp"
#include "core/port_caps.hpp"
#include "core/stream_telemetry.hpp"
#include "core/thread_spawn.hpp"

/* lwIP declares sockaddr_in in <sys/socket.h>, not <netinet/in.h>. */
#include <sys/socket.h>
#include <netinet/in.h>

namespace vstreamer
{

/*
 * Pad 0 (source): STREAM_DGRAM out.
 *
 * Takes media datagrams ([stream_media_header][SDU]) and hands each SDU on as it comes; the one
 * after a gap in the sequence is marked DISCONT. There is no FEC here: the link recovers loss
 * (winject-l3 runs FEC per upstream and delivers in sequence). Link reports go back to the sender
 * every telemetry_ms with two sets of counters: "udp" for the link (what reached the link's
 * receiver, before its recovery) and "fec" for the stream (what reached this receiver, after it).
 * Without link counters (set_link_counters) both are this receiver's own.
 */
class stream_receiver : public component_source
{
public:
    stream_receiver();
    ~stream_receiver() override;

    stream_receiver(const stream_receiver &) = delete;
    stream_receiver &operator=(const stream_receiver &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;

    /*
     * In-process datagrams instead of the listen socket; set before open() (then listen is not
     * needed). receive returns the next wire datagram, or nothing when none is ready, and must
     * not block: the receive thread polls it about every millisecond. send takes one reverse link
     * report. Both run on the receive thread. A received datagram is kept, not copied: its
     * shards are views into it until they are released.
     */
    using datagram_receive = std::function<std::optional<bfc::shared_sized_buffer>()>;
    using datagram_send = std::function<bool(const uint8_t *, size_t)>;
    int set_datagram_transport(datagram_receive receive, datagram_send send);

    /*
     * The link's own receive counters for the "udp" half of the link report and the queries:
     * frames that reached the link's receiver, frames missing from its sequence, and their
     * bytes, before the link's loss recovery (for winject-l3: the upstream's air RX counters).
     * Cumulative; a drop (the link restarted) starts a new count at the sender. Called on the
     * receive and query threads, so it must not block. Set before open().
     */
    struct link_counters
    {
        uint64_t packets = 0;
        uint64_t gaps = 0;
        uint64_t bytes = 0;
    };
    using link_counters_fn = std::function<bool(link_counters *)>;
    int set_link_counters(link_counters_fn fn);
    int output(component_pdu &out) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    [[nodiscard]] stream_link_report link_report_snapshot() const;
    void recv_thread_main();
    void stop_recv_thread();
    /* Returns true when the datagram is valid stream media (telemetry peer update). With owner,
     * data lies in it and is kept instead of copied. */
    bool ingest_datagram(const uint8_t *data, size_t len,
                         const bfc::shared_sized_buffer *owner = nullptr);
    void enqueue_payload_buffer(bfc::shared_sized_buffer &&payload, bool discont);

    static const std::vector<port_desc> &output_ports();

    mutable std::mutex mu;
    bool               opened = false;

    datagram_receive dgram_receive;
    datagram_send    dgram_send;
    link_counters_fn link_counters_src;
    std::string      listen_spec;
    int         max_datagram = 1472;
    int         recv_fd = -1;

    buffer_pool pool;

    std::mutex              q_mu;
    std::condition_variable q_cv;
    bool                    stopping = false;
    std::deque<component_pdu> pdu_queue;
    static constexpr size_t k_queue_depth = 4096;
    bool                    discont_next_ = false;

    /* Media datagrams taken and the stream sequence numbers missing between them. */
    std::atomic<uint64_t> media_packet_received {0};
    std::atomic<uint64_t> media_gap_count {0};

    std::atomic<uint64_t> recv_bytes {0};
    std::atomic<uint64_t> recv_wire_bytes {0};
    std::atomic<uint64_t> rx_truncated {0};
    std::atomic<uint64_t> rx_oversize {0};

    double   egress_rate_t0 = 0.;
    uint64_t egress_rate_bytes = 0;
    float    egress_kbps = 0.f;
    uint64_t recv_dropped = 0;
    uint64_t rx_bad_header = 0;

    uint16_t last_udp_seq = 0;
    bool     have_udp_seq = false;


    std::thread       recv_thread;
    thread_place      recv_place; /* keys recv_cpu / recv_prio */
    std::atomic<bool> recv_stop {false};

    std::atomic<int>  telemetry_ms {100};
    std::atomic<uint32_t> session_id {0};
    std::atomic<uint32_t> report_seq {0};
    std::atomic<uint64_t> telemetry_sent {0};
    std::atomic<uint64_t> telemetry_send_errors {0};
    std::atomic<uint64_t> telemetry_peer_changes {0};
    mutable std::mutex    peer_display_mu;
    std::string           telemetry_peer_str;

};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_STREAM_RECEIVER_HPP
