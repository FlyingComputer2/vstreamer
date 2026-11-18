#include "components/stream_sender.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstring>
#include <future>
#include <iterator>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "core/host_util.hpp"
#include "core/key_util.hpp"
#include "core/port_caps.hpp"
#include "core/stream_header.hpp"
#include "core/stream_media_header.hpp"
#include "core/stream_metrics.hpp"
#include "core/thread_spawn.hpp"

#if defined(ESP_PLATFORM)
#include "esp_task_wdt.h"
#endif

namespace vstreamer
{
namespace
{

/* Thread stacks on ESP-IDF; ignored on Linux. */
constexpr size_t k_send_stack_bytes = 12 * 1024;

size_t wire_packet_bytes(const bfc::shared_sized_buffer &buf)
{
    return buf.size();
}

}  // namespace

#if defined(ESP_PLATFORM)
struct stream_sender::send_thread_wdt
{
    send_thread_wdt()
    {
        const esp_err_t e = esp_task_wdt_add(nullptr);
        ok_ = (ESP_OK == e || ESP_ERR_INVALID_STATE == e);
    }
    ~send_thread_wdt()
    {
        if (ok_)
        {
            (void)esp_task_wdt_delete(nullptr);
        }
    }
    void feed()
    {
        if (ok_)
        {
            (void)esp_task_wdt_reset();
        }
    }

private:
    bool ok_ = false;
};
#else
struct stream_sender::send_thread_wdt
{
    void feed() {}
};
#endif

const std::vector<port_desc> &stream_sender::input_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::STREAM_DGRAM;
        p.caps.push_back(caps);
        /* RTP from rtp_h264_pay goes out unchanged: the payload is opaque here. */
        port_caps_entry rtp {};
        rtp.sdu_type = sdu_type_e::RTP;
        p.caps.push_back(rtp);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

stream_sender::stream_sender()
    : pool(static_cast<size_t>(1500), k_queue_packet_cap)
{
    std::random_device rd;
    tx_seq = static_cast<uint16_t>(rd());
}

size_t stream_sender::max_input_bytes()
{
    return static_cast<size_t>(k_wire_datagram_bytes) - k_media_header_len;
}

stream_sender::~stream_sender()
{
    close();
}

std::string stream_sender::name() const
{
    return "stream_sender";
}

bool stream_sender::wire_fits(size_t len)
{
    if (0 == len || len > static_cast<size_t>(k_wire_datagram_bytes))
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

/* One push evicts the oldest packets until there is room. */
void stream_sender::evict_for_room()
{
    uint64_t evicted = 0;
    {
        std::lock_guard<std::mutex> lock(q_mu);
        while (!queue.empty() && queue.size() >= k_queue_packet_cap)
        {
            queue_bytes -= wire_packet_bytes(queue.front().buf);
            queue.pop_front();
            ++evicted;
        }
    }
    if (evicted > 0)
    {
        queue_evicted.fetch_add(evicted, std::memory_order_relaxed);
        dropped.fetch_add(evicted, std::memory_order_relaxed);
    }
}

void stream_sender::queue_wire(bfc::shared_sized_buffer &&buf)
{
    {
        std::lock_guard<std::mutex> lock(q_mu);
        media_header_write(buf.u8(), tx_seq++);
        queue_bytes += buf.size();
        queue.push_back(queued_wire {std::move(buf), std::chrono::steady_clock::now()});
    }
    q_cv.notify_one();
}

void stream_sender::stop_send_thread()
{
    if (send_thread.joinable())
    {
        send_stop = true;
        q_cv.notify_all();
        send_thread.join();
        send_stop = false;
    }
}

void stream_sender::stop_reactor()
{
    if (!reactor_running_)
    {
        udp_reactor_.reset();
        queue_reactor_.reset();
        return;
    }
    auto removed = std::make_shared<std::promise<void>>();
    auto done = removed->get_future();
    if (queue_reactor_)
    {
        if (rx_registered_)
        {
            (void)queue_reactor_->remove_read_rdy(*transport_rx_);
        }
        // Wait past any context snapshot already being processed.
        if (!queue_reactor_->is_reactor_thread())
        {
            queue_reactor_->wake_up([removed] { removed->set_value(); });
            done.wait();
        }
        else if (transport_rx_)
        {
            // This pass may still hold the queue's raw pointer; keep it alive past it.
            queue_reactor_->wake_up([rx = transport_rx_] {});
        }
    }
    else if (send_fd >= 0)
    {
        (void)udp_reactor_->rem_read_rdy(send_fd, [removed] { removed->set_value(); });
        done.wait();
    }
    udp_reactor_.reset();
    queue_reactor_.reset();
    reactor_running_ = false;
    rx_registered_ = false;
}

void stream_sender::ingest_link_report(const uint8_t *data, size_t len)
{
    stream_header hdr {};
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
    stream_link_report rep {};
    if (stream_header_parse(data, len, &hdr, &payload, &payload_len) < 0 ||
        hdr.is_fec || hdr.is_stream_data ||
        stream_link_report_decode(payload, payload_len, &rep) < 0)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        peer_reports_rejected++;
        return;
    }
    rep.report_seq = hdr.sequence_number;
    handle_link_report(rep);
}

void stream_sender::on_queue_report_read()
{
    // CV reactors notify once per wake, so drain all available entries.
    while (auto wire = transport_rx_->pop())
    {
        ingest_link_report(wire->u8(), wire->size());
    }
}

void stream_sender::on_telemetry_socket_read()
{
    uint8_t buf[512];
    const int fd = send_fd;
    if (fd < 0)
    {
        return;
    }
    for (;;)
    {
        const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC);
        if (n <= 0)
        {
            break;
        }
        ingest_link_report(buf, static_cast<size_t>(n));
    }
}

void stream_sender::handle_link_report(const stream_link_report &report)
{
    std::lock_guard<std::mutex> lock(peer_mu);
    int64_t observed_ms = -1;
    if (peer_have && report.session_id == peer_session)
    {
        const int16_t diff =
            static_cast<int16_t>(static_cast<uint16_t>(report.report_seq) -
                                 static_cast<uint16_t>(peer_last_seq));
        if (diff <= 0)
        {
            peer_reports_rejected++;
            return;
        }
        if (diff > 1)
        {
            peer_reports_lost += static_cast<uint64_t>(diff - 1);
        }
        if (peer_have_timestamp && diff > 0 && report.timestamp_us > peer_last_timestamp_us)
        {
            const uint64_t delta_us = report.timestamp_us - peer_last_timestamp_us;
            observed_ms =
                static_cast<int64_t>(delta_us / static_cast<uint64_t>(diff) / 1000ULL);
        }
    }
    else
    {
        observed_ms = -1;
        peer_have_timestamp = false;
    }
    peer_observed_interval_ms = observed_ms;
    peer_report = report;
    peer_session = report.session_id;
    peer_last_seq = report.report_seq;
    peer_last_timestamp_us = report.timestamp_us;
    peer_have_timestamp = true;
    peer_have = true;
    peer_report_at = std::chrono::steady_clock::now();
    peer_reports_received++;
}

stream_peer_link stream_sender::peer_link_snapshot() const
{
    stream_peer_link snap {};
    std::lock_guard<std::mutex> lock(peer_mu);
    snap.have = peer_have;
    snap.report = peer_report;
    snap.reports_received = peer_reports_received;
    snap.reports_lost = peer_reports_lost;
    snap.reports_rejected = peer_reports_rejected;
    snap.observed_interval_ms = peer_have ? peer_observed_interval_ms : -1;
    if (!peer_have)
    {
        snap.age_ms = -1;
        return snap;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - peer_report_at)
                        .count();
    snap.age_ms = ms;
    return snap;
}

void stream_sender::send_thread_main()
{
    send_thread_wdt wdt;
    sockaddr_in dst {};
    bool        dst_ok = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        dst_ok = have_dst;
        if (dst_ok)
        {
            std::memcpy(&dst, &dst_addr, sizeof(dst));
        }
    }

    while (!send_stop)
    {
        wdt.feed();

        bfc::shared_sized_buffer wire_buf;
        {
            /* Wake for a new enqueue, or the watchdog. */
            std::unique_lock<std::mutex> lock(q_mu);
            q_cv.wait_for(lock, std::chrono::milliseconds(50),
                          [this] { return send_stop || !queue.empty(); });
            if (send_stop)
            {
                break;
            }
            if (queue.empty())
            {
                continue;
            }
            const int64_t late_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - queue.front().at)
                                        .count();
            if (late_us > send_late_max_us.load(std::memory_order_relaxed))
            {
                send_late_max_us.store(late_us, std::memory_order_relaxed);
            }
            wire_buf = std::move(queue.front().buf);
            queue_bytes -= wire_packet_bytes(wire_buf);
            queue.pop_front();
        }

        if (wire_buf.empty())
        {
            continue;
        }

        if (!transport_tx_ && (!dst_ok || send_fd < 0))
        {
            continue;
        }

        ssize_t n = 0;
        if (transport_tx_)
        {
            n = static_cast<ssize_t>(wire_buf.size());
            /* push returns the prior occupancy; at capacity it evicted the oldest packet. */
            if (transport_tx_->push(std::move(wire_buf)) >= transport_tx_->capacity())
            {
                transport_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            queue_reactor_->wake_up();
        }
        else
        {
            n = sendto(send_fd, wire_buf.u8(), wire_buf.size(), 0,
                       reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
        }
        if (n < 0)
        {
            continue;
        }
        std::lock_guard<std::mutex> metrics_lock(metrics_mu);
        pkts_sent.fetch_add(1, std::memory_order_relaxed);
        bytes_sent.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    }
}

int stream_sender::open()
{
    /* Snapshot the transport config, then resolve and bind without holding mu: a hostname
     * lookup can block, and query()/configure() must not wait on it. */
    std::string udp_spec, tx_name, rx_name, reactor_name;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return 0;
        }
        udp_spec = udp_transport_;
        tx_name = queue_transport_tx_;
        rx_name = queue_transport_rx_;
        reactor_name = reactor_name_;
    }
    const bool queue_mode = !tx_name.empty() || !rx_name.empty();
    if (reactor_name.empty() ||
        (queue_mode ? (!udp_spec.empty() || tx_name.empty() || rx_name.empty())
                    : udp_spec.empty()))
    {
        return -EINVAL;
    }

    std::shared_ptr<stream_transport_queue> tx, rx;
    std::shared_ptr<default_reactor> udp_reactor;
    std::shared_ptr<bfc::cv_reactor<>> queue_reactor;
    if (queue_mode)
    {
        tx = stream_transport_queue_registry::get_instance().get(tx_name);
        rx = stream_transport_queue_registry::get_instance().get(rx_name);
        if (!tx || !rx)
        {
            return -ENOENT;
        }
        if (tx == rx || tx->capacity() == 0 || rx->capacity() == 0)
        {
            return -EINVAL;
        }
        queue_reactor = bfc::cv_reactor_registry::get_instance().get(reactor_name);
        if (!queue_reactor)
        {
            return -ENOENT;
        }
    }
    else
    {
        udp_reactor = default_reactor_registry::get_instance().get(reactor_name);
        if (!udp_reactor)
        {
            return -ENOENT;
        }
    }

    int fd = -1;
    sockaddr_in dst {};
    if (!queue_mode)
    {
        char host[128];
        int port = 0;
        if (parse_host_port(udp_spec, host, sizeof(host), &port) < 0 ||
            resolve_ipv4_destination(host, port, &dst) < 0)
        {
            return -EINVAL;
        }
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            return -errno;
        }
        constexpr int k_sock_buf = 16 * 1024 * 1024;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &k_sock_buf, sizeof(k_sock_buf));
        sockaddr_in bind_addr {};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0)
        {
            const int err = errno;
            ::close(fd);
            return -err;
        }
    }

    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        /* A concurrent open() won. */
        if (fd >= 0)
        {
            ::close(fd);
        }
        return 0;
    }
    send_fd = fd;
    dst_addr = dst;
    have_dst = !queue_mode;
    transport_tx_ = std::move(tx);
    transport_rx_ = std::move(rx);
    udp_reactor_ = std::move(udp_reactor);
    queue_reactor_ = std::move(queue_reactor);
    {
        std::lock_guard<std::mutex> plock(peer_mu);
        peer_have = false;
        peer_report = {};
        peer_session = 0;
        peer_last_seq = 0;
        peer_reports_received = 0;
        peer_reports_lost = 0;
        peer_reports_rejected = 0;
    }

    const bool reverse_path = queue_reactor_ ? static_cast<bool>(transport_rx_) : (send_fd >= 0);
    bool       registered = true;
    if (queue_reactor_ && reverse_path)
    {
        registered = queue_reactor_->add_read_rdy(*transport_rx_,
                                                 [this] { on_queue_report_read(); });
        rx_registered_ = registered;
        queue_reactor_->wake_up(); // Also handles data queued before open.
    }
    else if (udp_reactor_ && reverse_path)
    {
        registered = udp_reactor_->add_read_rdy(send_fd,
                                                [this] { on_telemetry_socket_read(); });
    }
    if (!registered)
    {
        if (send_fd >= 0)
        {
            ::close(send_fd);
        }
        send_fd = -1;
        have_dst = false;
        transport_tx_.reset();
        transport_rx_.reset();
        udp_reactor_.reset();
        queue_reactor_.reset();
        return -EIO;
    }
    reactor_running_ = reverse_path;
    opened = true;
    accepting.store(true, std::memory_order_release);
    send_stop = false;
    send_thread = spawn_thread(send_place.opts("vs_send", k_send_stack_bytes),
                               &stream_sender::send_thread_main, this);
    return 0;
}

void stream_sender::close()
{
    accepting.store(false, std::memory_order_release);
    stop_send_thread();
    stop_reactor();

    std::lock_guard<std::mutex> lock(mu);
    if (send_fd >= 0)
    {
        ::close(send_fd);
        send_fd = -1;
    }
    have_dst = false;
    transport_tx_.reset();
    transport_rx_.reset();
    opened = false;

    {
        std::lock_guard<std::mutex> plock(peer_mu);
        peer_have = false;
    }

    {
        std::lock_guard<std::mutex> qlock(q_mu);
        while (!queue.empty())
        {
            queue.pop_front();
        }
        queue_bytes = 0;
    }
    q_cv.notify_all();
}

int stream_sender::ingest_app_sdu(const uint8_t *data, size_t len, bool pdu_no_evict)
{
    if (!accepting.load(std::memory_order_acquire))
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (len > max_input_bytes())
    {
        oversized.fetch_add(1, std::memory_order_relaxed);
        dropped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (pdu_no_evict)
    {
        std::lock_guard<std::mutex> lock(q_mu);
        if (!queue.empty() && queue.size() >= k_queue_packet_cap)
        {
            queue_full_rejects.fetch_add(1, std::memory_order_relaxed);
            return -EAGAIN;
        }
    }
    const size_t wire_len = k_media_header_len + len;
    if (!wire_fits(wire_len))
    {
        return 0;
    }
    bfc::shared_sized_buffer buf = pool.acquire(wire_len);
    if (0 == buf.capacity() || buf.size() != wire_len)
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    std::memcpy(buf.u8() + k_media_header_len, data, len);
    if (!pdu_no_evict)
    {
        evict_for_room();
    }
    queue_wire(std::move(buf));
    std::lock_guard<std::mutex> lock(metrics_mu);
    ++sdu_pkts_sent;
    sdu_bytes_sent += len;
    return 0;
}

int stream_sender::input(component_pdu &&in)
{
    if (0 != in.port ||
        (in.sdu_type != sdu_type_e::STREAM_DGRAM && in.sdu_type != sdu_type_e::RTP))
    {
        return -EINVAL;
    }
    if (in.sdu.empty())
    {
        return -EINVAL;
    }
    return ingest_app_sdu(in.sdu.u8(), in.sdu.size(), true);
}

namespace
{

bool parse_on_off(std::string_view value, bool *out)
{
    if (nullptr == out)
    {
        return false;
    }
    if ("on" == value || "ON" == value)
    {
        *out = true;
        return true;
    }
    if ("off" == value || "OFF" == value)
    {
        *out = false;
        return true;
    }
    return false;
}

}  // namespace

int stream_sender::configure(std::string_view key, std::string_view value)
{
    {
        std::lock_guard<std::mutex> lock(mu);
        if (const int pr = thread_place_configure(&send_place, "send", key, value, opened);
            -ENOTSUP != pr)
        {
            return pr;
        }
    }
    if ("reactor" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return -EBUSY;
        }
        reactor_name_.assign(value.data(), value.size());
        return 0;
    }
    if ("udp_transport" == key || "queue_transport_tx" == key || "queue_transport_rx" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return -EBUSY;
        }
        if (!value.empty())
        {
            if (key == "udp_transport")
            {
                if (!queue_transport_tx_.empty() || !queue_transport_rx_.empty())
                {
                    return -EINVAL;
                }
            }
            else if (!udp_transport_.empty())
            {
                return -EINVAL;
            }
        }
        auto &target = key == "udp_transport" ? udp_transport_
                     : key == "queue_transport_tx" ? queue_transport_tx_ : queue_transport_rx_;
        target.assign(value.data(), value.size());
        return 0;
    }
    return -ENOTSUP;
}

int stream_sender::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    if (const int pr = thread_place_query(send_place, "send", key, value); -ENOTSUP != pr)
    {
        return pr;
    }

    int r = port_caps_query(input_ports(), true, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }

    if ("reactor" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = reactor_name_;
        return 0;
    }
    if ("udp_transport" == key || "queue_transport_tx" == key || "queue_transport_rx" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = key == "udp_transport" ? udp_transport_
               : key == "queue_transport_tx" ? queue_transport_tx_ : queue_transport_rx_;
        return 0;
    }
    if ("pkt_sent" == key || "byt_sent" == key)
    {
        std::lock_guard<std::mutex> lock(metrics_mu);
        *value = stream_metric_format(
            stream_metric_timestamp_ms(),
            key == "pkt_sent" ? pkts_sent.load(std::memory_order_relaxed)
                              : bytes_sent.load(std::memory_order_relaxed),
            key == "pkt_sent" ? sdu_pkts_sent : sdu_bytes_sent);
        return 0;
    }
    if ("peer_udp_pkt_recv" == key || "peer_fec_pkt_recv" == key ||
        "peer_byt_recv" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        if (!peer_have ||
            (key == "peer_byt_recv" && !peer_report.has_byte_totals))
        {
            return -ENODATA;
        }
        const auto &c = peer_report.counters;
        const uint64_t ts = peer_report.timestamp_us / 1000;
        if (key == "peer_udp_pkt_recv")
        {
            *value = stream_metric_format(ts, c.udp_packet_received,
                                          c.udp_gap_count);
        }
        else if (key == "peer_fec_pkt_recv")
        {
            *value = stream_metric_format(ts, c.fec_packet_received,
                                          c.fec_gap_count);
        }
        else
        {
            *value = stream_metric_format(ts, c.udp_bytes_received,
                                          c.fec_bytes_received);
        }
        return 0;
    }
    if ("stats" == key)
    {
        stream_peer_link peer {};
        {
            std::lock_guard<std::mutex> plock(peer_mu);
            peer.reports_received = peer_reports_received;
            peer.reports_lost = peer_reports_lost;
            peer.reports_rejected = peer_reports_rejected;
        }
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "pkts=%" PRIu64 " bytes=%" PRIu64 " dropped=%" PRIu64
                      " oversized=%" PRIu64 " peer{rcv=%" PRIu64 " lost=%" PRIu64
                      " rej=%" PRIu64 "}",
                      pkts_sent.load(std::memory_order_relaxed),
                      bytes_sent.load(std::memory_order_relaxed),
                      dropped.load(std::memory_order_relaxed),
                      oversized.load(std::memory_order_relaxed), peer.reports_received,
                      peer.reports_lost, peer.reports_rejected);
        *value = buf;
        return 0;
    }
    if ("send_late_max_ms" == key)
    {
        double ms = 0.0;
        const int r = query(query_key(key), &ms);
        if (0 == r)
        {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%.1f", ms);
            *value = buf;
        }
        return r;
    }
    return query_typed_as_string(key, value);
}

int stream_sender::query(uint64_t key, uint64_t *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    switch (key)
    {
    case query_key("peer_udp_packet_received"):
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        *value = peer_have ? peer_report.counters.udp_packet_received : 0;
        return 0;
    }
    case query_key("peer_fec_packet_received"):
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        *value = peer_have ? peer_report.counters.fec_packet_received : 0;
        return 0;
    }
    case query_key("peer_udp_gap_count"):
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        *value = peer_have ? peer_report.counters.udp_gap_count : 0;
        return 0;
    }
    case query_key("peer_fec_gap_count"):
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        *value = peer_have ? peer_report.counters.fec_gap_count : 0;
        return 0;
    }
    case query_key("peer_reports_received"):
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        *value = peer_reports_received;
        return 0;
    }
    case query_key("peer_reports_lost"):
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        *value = peer_reports_lost;
        return 0;
    }
    case query_key("peer_reports_rejected"):
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        *value = peer_reports_rejected;
        return 0;
    }
    case query_key("peer_session"):
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        *value = peer_have ? peer_session : 0U;
        return 0;
    }
    case query_key("oversized"):
        *value = oversized.load(std::memory_order_relaxed);
        return 0;
    case query_key("max_datagram"):
        *value = static_cast<uint64_t>(k_wire_datagram_bytes);
        return 0;
    case query_key("max_input"):
        *value = max_input_bytes();
        return 0;
    case query_key("pool_misses"):
        *value = pool.misses();
        return 0;
    case query_key("queue_packets"):
    {
        std::lock_guard<std::mutex> lock(q_mu);
        *value = queue.size();
        return 0;
    }
    case query_key("queue_bytes"):
    {
        std::lock_guard<std::mutex> lock(q_mu);
        *value = queue_bytes;
        return 0;
    }
    case query_key("dropped"):
        *value = dropped.load(std::memory_order_relaxed);
        return 0;
    case query_key("queue_evicted"):
        *value = queue_evicted.load(std::memory_order_relaxed);
        return 0;
    case query_key("transport_dropped"):
        *value = transport_dropped.load(std::memory_order_relaxed);
        return 0;
    case query_key("queue_full_rejects"):
        *value = queue_full_rejects.load(std::memory_order_relaxed);
        return 0;
    default:
        return -ENOTSUP;
    }
}

int stream_sender::query(uint64_t key, int64_t *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    switch (key)
    {
    case query_key("peer_report_age_ms"):
        *value = peer_link_snapshot().age_ms;
        return 0;
    case query_key("peer_report_interval_ms"):
        *value = peer_link_snapshot().observed_interval_ms;
        return 0;
    default:
        return -ENOTSUP;
    }
}

int stream_sender::query(uint64_t key, double *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    switch (key)
    {
    case query_key("send_late_max_ms"):
        /* Since the previous query: the longest a datagram waited in the send queue. */
        *value = static_cast<double>(send_late_max_us.exchange(0, std::memory_order_relaxed)) /
                 1000.0;
        return 0;
    default:
        return -ENOTSUP;
    }
}

}  // namespace vstreamer
