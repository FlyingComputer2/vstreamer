#include "components/stream_sender.hpp"

#include "core/fec_spread.hpp"
#include "core/host_util.hpp"
#include "core/key_util.hpp"
#include "core/port_caps.hpp"
#include "core/stream_header.hpp"
#include "core/thread_spawn.hpp"

#include <cerrno>
#include <climits>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <future>
#include <iterator>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(ESP_PLATFORM)
#include "esp_task_wdt.h"
#endif

namespace vstreamer
{
namespace
{

/* Thread stacks on ESP-IDF; ignored on Linux. */
constexpr size_t k_send_stack_bytes = 12 * 1024;
constexpr size_t k_io_reactor_stack_bytes = 4 * 1024;

double now_sec()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void update_kbps_window(double &t0, uint64_t &acc, float &kbps, size_t nbytes)
{
    const double t = now_sec();
    if (t0 <= 0.)
    {
        t0 = t;
    }
    acc += nbytes;
    const double dt = t - t0;
    if (dt >= 0.5)
    {
        kbps = static_cast<float>(acc * 8.0 / dt / 1000.0);
        t0 = t;
        acc = 0;
    }
}

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
}

size_t stream_sender::queue_byte_limit() const
{
    std::lock_guard<std::mutex> lock(mu);
    /* ingress_kbps is the app rate; parity adds n/k on the wire. */
    const double rate_bps = static_cast<double>(ingress_kbps) * 125.0 *
                            static_cast<double>(effective_fec_n()) /
                            static_cast<double>(effective_fec_k());
    /* Spread holds each block's shards back for up to fec_spread_ms on top of the normal
     * queueing, so the queue holds that much more traffic before it rejects SDUs. */
    const int    window_ms = queue_ms + fec_spread_ms.load(std::memory_order_relaxed);
    const size_t scaled =
        static_cast<size_t>(rate_bps * static_cast<double>(window_ms) / 1000.0);
    /* The queue absorbs bursts. A keyframe and all of its parity are enqueued before the send
     * thread runs; evicting from the head then drops unsent data shards of a block whose first
     * shards are already on the wire, and the receiver rebuilds them from parity only after
     * later packets, which the depayloader sees as reordering. */
    const size_t unpaced_min =
        k_queue_min_unpaced_app_bytes * static_cast<size_t>(effective_fec_n()) /
        static_cast<size_t>(effective_fec_k());
    size_t wire_min = std::max<size_t>(256U * 1024U, unpaced_min);
    const int spread_ms = fec_spread_ms.load(std::memory_order_relaxed);
    if (spread_ms > 0 && queue_ms > 0)
    {
        wire_min = wire_min * static_cast<size_t>(queue_ms + spread_ms) /
                   static_cast<size_t>(queue_ms);
    }
    return std::max<size_t>(wire_min, scaled);
}

int stream_sender::effective_fec_k() const
{
    if (fec_mode == fec_mode_e::none)
    {
        return 1;
    }
    return fec_k;
}

int stream_sender::effective_fec_n() const
{
    if (fec_mode == fec_mode_e::none)
    {
        return 1;
    }
    return fec_n;
}

size_t stream_sender::max_fec_shard_bytes() const
{
    std::lock_guard<std::mutex> lock(mu);
    return stream_max_fec_shard(static_cast<size_t>(k_wire_datagram_bytes));
}

size_t stream_sender::max_raw_sdu_bytes() const
{
    std::lock_guard<std::mutex> lock(mu);
    return stream_max_raw_sdu(static_cast<size_t>(k_wire_datagram_bytes));
}

stream_sender::~stream_sender()
{
    close();
}

std::string stream_sender::name() const
{
    return "stream_sender";
}



bool stream_sender::queue_has_room_for_sdu() const
{
    size_t pkts = 1;
    size_t pkt_bytes = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (fec_mode_e::none == fec_mode)
        {
            pkt_bytes = k_stream_header_len + stream_max_raw_sdu(static_cast<size_t>(k_wire_datagram_bytes));
        }
        else
        {
            /* One SDU can close a block: its data shard plus all of the block's parity. */
            pkts = static_cast<size_t>(std::max(1, fec_n - fec_k + 1));
            pkt_bytes =
                k_stream_header_len + stream_max_fec_shard(static_cast<size_t>(k_wire_datagram_bytes));
        }
    }
    const size_t limit = queue_byte_limit();
    std::lock_guard<std::mutex> lock(q_mu);
    if (queue.empty())
    {
        return true;
    }
    return queue_bytes + pkts * pkt_bytes <= limit && queue.size() + pkts <= k_queue_packet_cap;
}

int stream_sender::try_enqueue_wire_copy(const uint8_t *data, size_t len, bool is_fec_shard,
                                         std::chrono::steady_clock::time_point release,
                                         bool force)
{
    if (nullptr == data || 0 == len)
    {
        return 0;
    }
    size_t payload_max = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        payload_max =
            is_fec_shard ? stream_max_fec_shard(static_cast<size_t>(k_wire_datagram_bytes))
                         : stream_max_raw_sdu(static_cast<size_t>(k_wire_datagram_bytes));
    }
    if (0 == payload_max || len > payload_max)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped.fetch_add(1, std::memory_order_relaxed);
        return -EINVAL;
    }
    const size_t wire_len = k_stream_header_len + len;
    const size_t limit = queue_byte_limit();

    {
        std::lock_guard<std::mutex> lock(q_mu);
        if (!force && !queue.empty() &&
            (queue_bytes + wire_len > limit || queue.size() >= k_queue_packet_cap))
        {
            return -EAGAIN;
        }
    }

    bfc::shared_sized_buffer buf = pool.acquire(wire_len);
    if (0 == buf.capacity() || buf.size() != wire_len)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped.fetch_add(1, std::memory_order_relaxed);
        return -ENOMEM;
    }
    /* sequence_number is stamped in send_thread_main() on every datagram. */
    stream_header hdr {};
    hdr.sequence_number = 0;
    hdr.is_fec = is_fec_shard;
    hdr.is_stream_data = true;
    hdr.ext_len = 0;
    stream_header_write(buf.u8(), hdr);
    std::memcpy(buf.u8() + k_stream_header_len, data, len);
    {
        std::lock_guard<std::mutex> lock(q_mu);
        queue_bytes += wire_len;
        /* Usually the newest release time, so the search ends at the back. */
        auto pos = queue.end();
        while (pos != queue.begin() && std::prev(pos)->release > release)
        {
            --pos;
        }
        queue.insert(pos, queued_wire {std::move(buf), release});
    }
    q_cv.notify_one();
    return 0;
}

void stream_sender::enqueue_wire_copy(const uint8_t *data, size_t len, bool is_fec_shard,
                                      std::chrono::steady_clock::time_point release)
{
    if (nullptr == data || 0 == len)
    {
        return;
    }
    size_t payload_max = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        payload_max =
            is_fec_shard ? stream_max_fec_shard(static_cast<size_t>(k_wire_datagram_bytes))
                         : stream_max_raw_sdu(static_cast<size_t>(k_wire_datagram_bytes));
    }
    if (0 == payload_max || len > payload_max)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const size_t wire_len = k_stream_header_len + len;
    const size_t limit = queue_byte_limit();

    bool evicted = false;
    {
        std::lock_guard<std::mutex> lock(q_mu);
        while (!queue.empty() &&
               (queue_bytes + wire_len > limit || queue.size() >= k_queue_packet_cap))
        {
            queue_bytes -= wire_packet_bytes(queue.front().buf);
            queue.pop_front();
            evicted = true;
        }
    }

    bfc::shared_sized_buffer buf = pool.acquire(wire_len);
    if (0 == buf.capacity() || buf.size() != wire_len)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    /* sequence_number is stamped in send_thread_main() on every datagram. */
    stream_header hdr {};
    hdr.sequence_number = 0;
    hdr.is_fec = is_fec_shard;
    hdr.is_stream_data = true;
    hdr.ext_len = 0;
    stream_header_write(buf.u8(), hdr);
    std::memcpy(buf.u8() + k_stream_header_len, data, len);
    {
        std::lock_guard<std::mutex> lock(q_mu);
        queue_bytes += wire_len;
        /* Usually the newest release time, so the search ends at the back. */
        auto pos = queue.end();
        while (pos != queue.begin() && std::prev(pos)->release > release)
        {
            --pos;
        }
        queue.insert(pos, queued_wire {std::move(buf), release});
    }
    if (evicted)
    {
        std::lock_guard<std::mutex> lock(mu);
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    q_cv.notify_one();
}

void stream_sender::enqueue_fec_air(std::vector<std::vector<uint8_t>> *air)
{
    if (nullptr == air)
    {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int  spread = fec_spread_ms.load(std::memory_order_relaxed);
    for (size_t j = 0; j < air->size(); j++)
    {
        auto &pkt = (*air)[j];
        enqueue_wire_copy(pkt.data(), pkt.size(), true,
                          t0 + fec_spread_offset(j, air->size(), spread));
    }
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

void stream_sender::stop_io_reactor()
{
    if (!io_reactor_running)
    {
        return;
    }
    const int fd = send_fd;
    if (fd >= 0)
    {
        (void)io_reactor_.rem_read_rdy(fd);
    }
    io_reactor_.stop();
    if (io_reactor_thread_.joinable())
    {
        io_reactor_thread_.join();
    }
    io_reactor_running = false;
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
        stream_header hdr {};
        const uint8_t *payload = nullptr;
        size_t         payload_len = 0;
        if (stream_header_parse(buf, static_cast<size_t>(n), &hdr, &payload, &payload_len) < 0)
        {
            std::lock_guard<std::mutex> lock(peer_mu);
            peer_reports_rejected++;
            continue;
        }
        if (hdr.is_fec || hdr.is_stream_data)
        {
            std::lock_guard<std::mutex> lock(peer_mu);
            peer_reports_rejected++;
            continue;
        }
        stream_link_report rep {};
        if (stream_link_report_decode(payload, payload_len, &rep) < 0)
        {
            std::lock_guard<std::mutex> lock(peer_mu);
            peer_reports_rejected++;
            continue;
        }
        rep.report_seq = hdr.sequence_number;
        handle_link_report(rep);
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

        int wait_ms = 50;
        {
            std::vector<std::vector<uint8_t>> tick_air;
            {
                std::lock_guard<std::mutex> flock(fec_mu);
                fec.on_tick(&tick_air);
                std::chrono::steady_clock::time_point dl;
                if (fec.next_deadline(&dl))
                {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        dl - std::chrono::steady_clock::now())
                                        .count();
                    const int until = static_cast<int>(std::max<int64_t>(0, ms));
                    wait_ms = std::min(50, until);
                    if (wait_ms < 1)
                    {
                        wait_ms = 1;
                    }
                }
            }
            if (!tick_air.empty())
            {
                enqueue_fec_air(&tick_air);
            }
        }

        bfc::shared_sized_buffer wire_buf;
        {
            /* Wake for the earliest release, a new enqueue, or the next FEC tick. */
            std::unique_lock<std::mutex> lock(q_mu);
            const auto tick_at =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
            for (;;)
            {
                wdt.feed();
                if (send_stop)
                {
                    break;
                }
                if (fec_deadline_changed)
                {
                    fec_deadline_changed = false;
                    break;
                }
                const auto t = std::chrono::steady_clock::now();
                if (!queue.empty() && queue.front().release <= t)
                {
                    break;
                }
                if (t >= tick_at)
                {
                    break;
                }
                auto until = tick_at;
                if (!queue.empty() && queue.front().release < until)
                {
                    until = queue.front().release;
                }
                q_cv.wait_until(lock, until);
            }
            if (send_stop)
            {
                break;
            }
            if (queue.empty() || queue.front().release > std::chrono::steady_clock::now())
            {
                continue;
            }
            const int64_t late_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - queue.front().release)
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

        if (!dst_ok || send_fd < 0)
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> gate(gate_mu);
            if (!send_enabled)
            {
                continue;
            }
            if (deadline_sec > 0 && now_sec() >= deadline_sec)
            {
                send_enabled = false;
                deadline_sec = 0;
                continue;
            }
        }

        if (wire_buf.size() >= k_stream_header_len)
        {
            const uint16_t seq = stream_sequence.fetch_add(1, std::memory_order_relaxed);
            stream_header_stamp_sequence(wire_buf.u8(), seq);
        }
        const ssize_t n = sendto(send_fd, wire_buf.u8(), wire_buf.size(), 0,
                                    reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
        if (n < 0)
        {
            continue;
        }
        pkts_sent.fetch_add(1, std::memory_order_relaxed);
        bytes_sent.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    }
}

int stream_sender::open()
{
    std::string spec_copy;
    int         k = 0;
    int         n = 0;
    int         timeout_ms = 0;
    int         mtu_local = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return 0;
        }
        if (stream_spec.empty())
        {
            return -EINVAL;
        }
        spec_copy = stream_spec;
        k = effective_fec_k();
        n = effective_fec_n();
        timeout_ms = fec_timeout_ms;
        mtu_local = mtu;
    }
    const size_t shard_bytes = max_fec_shard_bytes();
    const bool   raw_mode = (fec_mode == fec_mode_e::none);

    char host[128];
    int  port = 0;
    if (parse_host_port(spec_copy, host, sizeof(host), &port) < 0)
    {
        return -EINVAL;
    }

    bool telemetry = true;
    {
        std::lock_guard<std::mutex> lock(mu);
        telemetry = telemetry_on;
    }

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -errno;
    }

    constexpr int k_sock_buf = 16 * 1024 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &k_sock_buf, sizeof(k_sock_buf));

    sockaddr_in bind_addr {};
    bind_addr.sin_family = AF_INET;
    if (resolve_ipv4_bind_addr("0.0.0.0", true, &bind_addr.sin_addr) < 0)
    {
        ::close(fd);
        return -EINVAL;
    }
    bind_addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0)
    {
        const int err = errno;
        ::close(fd);
        return -err;
    }

    sockaddr_in dst_in {};
    if (resolve_ipv4_destination(host, port, &dst_in) < 0)
    {
        ::close(fd);
        return -EINVAL;
    }

    {
        std::lock_guard<std::mutex> flock(fec_mu);
        fec_oversized = 0;
        if (raw_mode)
        {
            fec.disable();
            std::fprintf(stderr, "stream_sender: fec mode=none (raw SDU path)\n");
        }
        else
        {
            if (0 == shard_bytes || !fec.init(k, n, timeout_ms, shard_bytes))
            {
                ::close(fd);
                return -EINVAL;
            }
            std::fprintf(stderr,
                         "stream_sender: fec k=%d n=%d timeout=%d ms mode=block (%s)\n", k, n,
                         timeout_ms, fec.impl_name());
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            ::close(fd);
            return 0;
        }
        send_fd = fd;
        std::memcpy(&dst_addr, &dst_in, sizeof(dst_in));
        have_dst = true;
        opened = true;

        {
            std::random_device rd;
            stream_sequence.store(static_cast<uint16_t>(rd()), std::memory_order_relaxed);
        }

        send_stop = false;
        send_thread = spawn_thread(send_place.opts("vs_send", k_send_stack_bytes),
                                   &stream_sender::send_thread_main, this);

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
        if (telemetry)
        {
            if (!io_reactor_.add_read_rdy(
                    send_fd,
                    [this]()
                    {
                        on_telemetry_socket_read();
                    }))
            {
                send_stop = true;
                stop_send_thread();
                ::close(send_fd);
                send_fd = -1;
                have_dst = false;
                opened = false;
                return -EIO;
            }
            io_reactor_running = true;
            const auto started = std::make_shared<std::promise<void>>();
            const std::future<void> started_fut = started->get_future();
            io_reactor_thread_ = spawn_thread(
                thread_opts {"vs_io", k_io_reactor_stack_bytes},
                [this, started]()
                {
                    /* Queue before run() so the callback runs only after run() sets
                     * m_running; open() waits on this before returning. */
                    io_reactor_.wake_up(
                        [started]()
                        {
                            started->set_value();
                        });
                    io_reactor_.run();
                });
            if (started_fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            {
                io_reactor_.stop();
                if (io_reactor_thread_.joinable())
                {
                    io_reactor_thread_.join();
                }
                io_reactor_running = false;
                send_stop = true;
                stop_send_thread();
                ::close(send_fd);
                send_fd = -1;
                have_dst = false;
                opened = false;
                return -ETIMEDOUT;
            }
        }

        std::fprintf(stderr, "stream_sender: udp://%s:%d mtu=%d\n", host, port, mtu_local);
    }
    return 0;
}

void stream_sender::close()
{
    stop_io_reactor();
    stop_send_thread();
    disable_fec();

    std::lock_guard<std::mutex> lock(mu);
    if (send_fd >= 0)
    {
        ::close(send_fd);
        send_fd = -1;
    }
    have_dst = false;
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

int stream_sender::ingest_app_sdu(const uint8_t *data, size_t len, size_t ingress_bytes,
                                  bool pdu_no_evict)
{
    bool oversized = false;
    bool opened_block = false;
    fec_mode_e mode = fec_mode_e::block;
    {
        std::lock_guard<std::mutex> lock(mu);
        mode = fec_mode;
    }
    /* Accept the whole SDU or none of it: once it is in an FEC block, every shard it produces
     * must reach the queue, or the block goes out without some of its data or parity. */
    if (pdu_no_evict && !queue_has_room_for_sdu())
    {
        queue_full_rejects.fetch_add(1, std::memory_order_relaxed);
        return -EAGAIN;
    }
    if (fec_mode_e::none == mode)
    {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!opened)
            {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
        }
        const auto release = std::chrono::steady_clock::now();
        if (pdu_no_evict)
        {
            const int er = try_enqueue_wire_copy(data, len, false, release, true);
            if (0 != er)
            {
                return er;
            }
        }
        else
        {
            enqueue_wire_copy(data, len, false, release);
        }
    }
    else
    {
        std::vector<std::vector<uint8_t>> air;
        {
            std::lock_guard<std::mutex> flock(fec_mu);
            if (!fec.enabled())
            {
                std::lock_guard<std::mutex> lock(mu);
                dropped.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            const uint64_t                        before = fec.oversized();
            std::chrono::steady_clock::time_point dl;
            const bool had_deadline = fec.next_deadline(&dl);
            fec.push_app(data, len, &air);
            oversized = fec.oversized() != before;
            opened_block = !had_deadline && fec.next_deadline(&dl);
        }
        if (!air.empty())
        {
            if (pdu_no_evict)
            {
                const auto t0 = std::chrono::steady_clock::now();
                const int  spread = fec_spread_ms.load(std::memory_order_relaxed);
                for (size_t j = 0; j < air.size(); j++)
                {
                    auto &pkt = air[j];
                    /* Room was checked above; a failure here is oversize or no buffer,
                     * which drops only this shard and is counted in dropped. */
                    (void)try_enqueue_wire_copy(pkt.data(), pkt.size(), true,
                                                t0 + fec_spread_offset(j, air.size(), spread),
                                                true);
                }
            }
            else
            {
                enqueue_fec_air(&air);
            }
        }
        if (opened_block)
        {
            /* The send thread runs the timeout flush; wake it to sleep toward this block's
             * deadline instead of an older, later one. */
            {
                std::lock_guard<std::mutex> lock(q_mu);
                fec_deadline_changed = true;
            }
            q_cv.notify_one();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        if (oversized)
        {
            fec_oversized++;
            dropped.fetch_add(1, std::memory_order_relaxed);
        }
        update_kbps_window(ingress_rate_t0, ingress_rate_bytes, ingress_kbps, ingress_bytes);
    }
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
    return ingest_app_sdu(in.sdu.u8(), in.sdu.size(), in.sdu.size(), true);
}


int stream_sender::set_enabled(bool on, int timeout_ms)
{
    std::lock_guard<std::mutex> gate(gate_mu);
    send_enabled = on;
    if (!on)
    {
        deadline_sec = 0;
        return 0;
    }
    if (timeout_ms <= 0)
    {
        deadline_sec = 0;
    }
    else
    {
        deadline_sec = now_sec() + (timeout_ms / 1000.0);
    }
    return 0;
}

bool stream_sender::enabled() const
{
    std::lock_guard<std::mutex> gate(gate_mu);
    if (!send_enabled)
    {
        return false;
    }
    if (deadline_sec > 0 && now_sec() >= deadline_sec)
    {
        return false;
    }
    return true;
}

int stream_sender::reinit_fec_if_active()
{
    int k = 0;
    int n = 0;
    int timeout_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!opened)
        {
            return 0;
        }
        k = effective_fec_k();
        n = effective_fec_n();
        timeout_ms = fec_timeout_ms;
    }
    fec_mode_e mode = fec_mode_e::block;
    {
        std::lock_guard<std::mutex> lock(mu);
        mode = fec_mode;
    }

    const size_t shard_bytes = max_fec_shard_bytes();
    if (fec_mode_e::block == mode)
    {
        if (0 == shard_bytes)
        {
            return -EINVAL;
        }
        if (k > n || k > rs_block_erasure::k_header_k_n_max ||
            n > rs_block_erasure::k_header_k_n_max)
        {
            return -EINVAL;
        }
    }

    /* Flush the partial block under the old k/n so no app packet is lost;
     * sdu_seq keeps counting so the receiver's order state stays valid. */
    std::vector<std::vector<uint8_t>> air;
    bool                              ok = true;
    {
        std::lock_guard<std::mutex> flock(fec_mu);
        fec.flush(&air);
        if (fec_mode_e::none == mode)
        {
            fec.disable();
        }
        else
        {
            ok = fec.init(k, n, timeout_ms, shard_bytes);
        }
    }
    enqueue_fec_air(&air);
    if (!ok)
    {
        return -EINVAL;
    }
    if (fec_mode_e::block == mode)
    {
        std::fprintf(stderr,
                     "stream_sender: fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms shard=%zu\n", k,
                     n, timeout_ms, shard_bytes);
    }
    return 0;
}

void stream_sender::disable_fec()
{
    std::vector<std::vector<uint8_t>> air;
    {
        std::lock_guard<std::mutex> flock(fec_mu);
        if (!fec.enabled())
        {
            return;
        }
        fec.flush(&air);
        fec.disable();
    }
    enqueue_fec_air(&air);
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
    if ("telemetry" == key)
    {
        bool on = false;
        if (!parse_on_off(value, &on))
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return -EBUSY;
        }
        telemetry_on = on;
        return 0;
    }
    if ("stream" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        stream_spec.assign(value.data(), value.size());
        return 0;
    }
    if ("mtu" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 200 || v > 1500 ||
            value.size() > 31)
        {
            return -EINVAL;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            mtu = static_cast<int>(v);
        }
        return 0;
    }
    if ("fec_spread_ms" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 0 || v > k_fec_spread_max_ms ||
            value.size() > 31)
        {
            return -EINVAL;
        }
        fec_spread_ms.store(static_cast<int>(v), std::memory_order_relaxed);
        /* Shards already queued under a longer spread would otherwise keep their old release
         * times: after a huge value they fill the queue for that long and stall the stream even
         * once the spread is lowered. Pull them in to the new spread; release times stay sorted
         * because the clamp is monotonic. */
        {
            const auto latest = std::chrono::steady_clock::now() + std::chrono::milliseconds(v);
            std::lock_guard<std::mutex> lock(q_mu);
            for (auto &q : queue)
            {
                q.release = std::min(q.release, latest);
            }
        }
        q_cv.notify_one();
        return 0;
    }
    if ("queue_ms" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 10 || v > 2000 || value.size() > 31)
        {
            return -EINVAL;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            queue_ms = static_cast<int>(v);
        }
        return 0;
    }
    if ("fec" == key)
    {
        std::string mode(value.data(), value.size());
        if ("none" == mode || "NONE" == mode)
        {
            {
                std::lock_guard<std::mutex> lock(mu);
                fec_mode = fec_mode_e::none;
            }
            return reinit_fec_if_active();
        }
        if ("block" == mode || "RS_BLOCK_ERASURE" == mode)
        {
            {
                std::lock_guard<std::mutex> lock(mu);
                fec_mode = fec_mode_e::block;
            }
            return reinit_fec_if_active();
        }
        return -EINVAL;
    }
    if ("fec_k" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < rs_block_erasure::k_header_k_n_min ||
            v > rs_block_erasure::k_header_k_n_max || value.size() > 31)
        {
            return -EINVAL;
        }
        int old_k = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (static_cast<int>(v) > fec_n)
            {
                return -EINVAL;
            }
            old_k = fec_k;
            fec_k = static_cast<int>(v);
            fec_mode = fec_mode_e::block;
        }
        if (reinit_fec_if_active() < 0)
        {
            std::lock_guard<std::mutex> lock(mu);
            fec_k = old_k;
            return -EINVAL;
        }
        return 0;
    }
    if ("fec_n" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < rs_block_erasure::k_header_k_n_min ||
            v > rs_block_erasure::k_header_k_n_max || value.size() > 31)
        {
            return -EINVAL;
        }
        int old_n = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (fec_k > static_cast<int>(v))
            {
                return -EINVAL;
            }
            old_n = fec_n;
            fec_n = static_cast<int>(v);
            fec_mode = fec_mode_e::block;
        }
        if (reinit_fec_if_active() < 0)
        {
            std::lock_guard<std::mutex> lock(mu);
            fec_n = old_n;
            return -EINVAL;
        }
        return 0;
    }
    if ("fec_timeout_ms" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 0 || v > 60'000 || value.size() > 31)
        {
            return -EINVAL;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            fec_timeout_ms = static_cast<int>(v);
        }
        return reinit_fec_if_active();
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

    if ("stream" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = stream_spec;
        return 0;
    }
    if ("telemetry" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = telemetry_on ? "on" : "off";
        return 0;
    }
    if ("peer_udp_packet_received" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64,
                      peer_have ? peer_report.counters.udp_packet_received : 0);
        *value = buf;
        return 0;
    }
    if ("peer_fec_packet_received" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64,
                      peer_have ? peer_report.counters.fec_packet_received : 0);
        *value = buf;
        return 0;
    }
    if ("peer_udp_gap_count" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, peer_have ? peer_report.counters.udp_gap_count : 0);
        *value = buf;
        return 0;
    }
    if ("peer_fec_gap_count" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, peer_have ? peer_report.counters.fec_gap_count : 0);
        *value = buf;
        return 0;
    }
    if ("peer_report_age_ms" == key)
    {
        const stream_peer_link snap = peer_link_snapshot();
        char                   buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRId64, snap.age_ms);
        *value = buf;
        return 0;
    }
    if ("peer_reports_received" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, peer_reports_received);
        *value = buf;
        return 0;
    }
    if ("peer_reports_lost" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, peer_reports_lost);
        *value = buf;
        return 0;
    }
    if ("peer_reports_rejected" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, peer_reports_rejected);
        *value = buf;
        return 0;
    }
    if ("peer_report_interval_ms" == key)
    {
        const stream_peer_link snap = peer_link_snapshot();
        char                   buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRId64, snap.observed_interval_ms);
        *value = buf;
        return 0;
    }
    if ("peer_session" == key)
    {
        std::lock_guard<std::mutex> lock(peer_mu);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%" PRIu32, peer_have ? peer_session : 0U);
        *value = buf;
        return 0;
    }
    if ("in_rate" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(ingress_kbps));
        *value = buf;
        return 0;
    }
    if ("fec_oversized" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, fec_oversized);
        *value = buf;
        return 0;
    }
    if ("max_datagram" == key)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", k_wire_datagram_bytes);
        *value = buf;
        return 0;
    }
    if ("max_input" == key)
    {
        fec_mode_e mode = fec_mode_e::block;
        {
            std::lock_guard<std::mutex> lock(mu);
            mode = fec_mode;
        }
        const int dg = k_wire_datagram_bytes;
        size_t max_in = 0;
        if (fec_mode_e::none == mode)
        {
            max_in = stream_max_raw_sdu(static_cast<size_t>(dg));
        }
        else
        {
            std::lock_guard<std::mutex> flock(fec_mu);
            if (!fec.enabled())
            {
                return -EINVAL;
            }
            max_in = fec.max_original();
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%zu", max_in);
        *value = buf;
        return 0;
    }
    if ("fec_k" == key || "fec_n" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        const int v = ("fec_k" == key) ? fec_k : fec_n;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", v);
        *value = buf;
        return 0;
    }
    if ("fec_mode" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = (fec_mode == fec_mode_e::none) ? "none" : "block";
        return 0;
    }
    if ("stats" == key)
    {
        bool fec_on = false;
        {
            std::lock_guard<std::mutex> flock(fec_mu);
            fec_on = fec.enabled();
        }
        stream_peer_link peer {};
        {
            std::lock_guard<std::mutex> plock(peer_mu);
            peer.have = peer_have;
            if (peer_have)
            {
                peer.report = peer_report;
            }
            peer.reports_received = peer_reports_received;
            peer.reports_lost = peer_reports_lost;
            peer.reports_rejected = peer_reports_rejected;
        }
        std::lock_guard<std::mutex> lock(mu);
        char buf[480];
        if (fec_on)
        {
            std::snprintf(buf, sizeof(buf),
                          "pkts=%" PRIu64 " bytes=%" PRIu64 " dropped=%" PRIu64
                          " in_rate=%.1f fec_oversized=%" PRIu64 " fec=k=%d,n=%d"
                          " peer{rcv=%" PRIu64 " lost=%" PRIu64 " rej=%" PRIu64 "}",
                          pkts_sent.load(std::memory_order_relaxed),
                          bytes_sent.load(std::memory_order_relaxed),
                          dropped.load(std::memory_order_relaxed),
                          static_cast<double>(ingress_kbps), fec_oversized, fec_k, fec_n,
                          peer.reports_received, peer.reports_lost, peer.reports_rejected);
        }
        else
        {
            std::snprintf(buf, sizeof(buf),
                          "pkts=%" PRIu64 " bytes=%" PRIu64 " dropped=%" PRIu64
                          " in_rate=%.1f peer{rcv=%" PRIu64 " lost=%" PRIu64 " rej=%" PRIu64 "}",
                          pkts_sent.load(std::memory_order_relaxed),
                          bytes_sent.load(std::memory_order_relaxed),
                          dropped.load(std::memory_order_relaxed),
                          static_cast<double>(ingress_kbps), peer.reports_received,
                          peer.reports_lost, peer.reports_rejected);
        }
        *value = buf;
        return 0;
    }
    if ("pool_misses" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, pool.misses());
        *value = buf;
        return 0;
    }
    if ("queue_bytes" == key)
    {
        std::lock_guard<std::mutex> lock(q_mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%zu", queue_bytes);
        *value = buf;
        return 0;
    }
    if ("queue_byte_limit" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%zu", queue_byte_limit());
        *value = buf;
        return 0;
    }
    if ("queue_ms" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", queue_ms);
        *value = buf;
        return 0;
    }
    if ("fec_spread_ms" == key)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", fec_spread_ms.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    if ("send_late_max_ms" == key)
    {
        /* Since the previous query: how far past its release time a datagram left. */
        const int64_t us = send_late_max_us.exchange(0, std::memory_order_relaxed);
        char          buf[24];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(us) / 1000.0);
        *value = buf;
        return 0;
    }
    if ("dropped" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, dropped.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    if ("queue_full_rejects" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64,
                      queue_full_rejects.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
