#include "components/stream_sender.hpp"

#include "core/host_util.hpp"
#include "core/key_util.hpp"
#include "core/stream_header.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <poll.h>
#include <random>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace vstreamer
{
namespace
{

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

size_t wire_packet_bytes(const data_packet &pkt)
{
    const sock_data &sd = data_packet::cast<sock_data>(pkt);
    return sd.buf.size();
}

/* `local` = [host:]port, port 0..65535 (0 = ephemeral). Empty = 0.0.0.0:0. */
int parse_local_spec(std::string_view spec, std::string *host, int *port)
{
    if (spec.empty())
    {
        *host = "0.0.0.0";
        *port = 0;
        return 0;
    }
    const size_t colon = spec.rfind(':');
    const std::string_view port_str =
        (std::string_view::npos == colon) ? spec : spec.substr(colon + 1);
    const std::string_view host_str =
        (std::string_view::npos == colon) ? std::string_view {} : spec.substr(0, colon);
    int64_t p = 0;
    if (port_str.empty() || port_str.size() > 5 || key_parse_i64(port_str, &p) < 0 || p < 0 ||
        p > 65535 || host_str.size() >= 128)
    {
        return -EINVAL;
    }
    *host = host_str.empty() ? "0.0.0.0" : std::string(host_str);
    *port = static_cast<int>(p);
    return 0;
}

}  // namespace

stream_sender::stream_sender()
    : pool(static_cast<size_t>(1500), k_queue_packet_cap)
{
}

size_t stream_sender::queue_byte_limit() const
{
    std::lock_guard<std::mutex> lock(mu);
    const int cap_kbps = max_wire_kbps.load(std::memory_order_relaxed);
    double    rate_bps = 0.;
    if (cap_kbps > 0)
    {
        rate_bps = static_cast<double>(cap_kbps) * 125.0;
    }
    else
    {
        /* ingress_kbps is the app rate; parity adds n/k on the wire. */
        rate_bps = static_cast<double>(ingress_kbps) * 125.0 *
                   static_cast<double>(effective_fec_n()) /
                   static_cast<double>(effective_fec_k());
    }
    const size_t scaled =
        static_cast<size_t>(rate_bps * static_cast<double>(queue_ms) / 1000.0);
    if (cap_kbps > 0)
    {
        /* Paced: the cap bounds how stale queued data may get. */
        return std::max<size_t>(k_queue_min_paced_bytes, scaled);
    }
    /* Unpaced: the queue only absorbs bursts. A keyframe and all of its parity are enqueued
     * before the send thread runs; evicting from the head then drops unsent data shards of a
     * block whose first shards are already on the wire, and the receiver rebuilds them from
     * parity only after later packets, which the depayloader sees as reordering. */
    const size_t unpaced_min =
        k_queue_min_unpaced_app_bytes * static_cast<size_t>(effective_fec_n()) /
        static_cast<size_t>(effective_fec_k());
    const size_t wire_min = std::max<size_t>(256U * 1024U, unpaced_min);
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
    return stream_max_fec_shard(static_cast<size_t>(max_datagram));
}

size_t stream_sender::max_raw_sdu_bytes() const
{
    std::lock_guard<std::mutex> lock(mu);
    return stream_max_raw_sdu(static_cast<size_t>(max_datagram));
}

stream_sender::~stream_sender()
{
    close();
}

std::string stream_sender::name() const
{
    return "stream_sender";
}

media_kind_e stream_sender::input_kind() const
{
    return media_kind_e::UNKNOWN;
}

packet_kind_e stream_sender::input_packet_kind(uint8_t port) const
{
    if (0 != port)
    {
        return packet_kind_e::UNKNOWN;
    }
    return packet_kind_e::SOCK;
}

void stream_sender::enqueue_wire_copy(const uint8_t *data, size_t len, bool is_fec_shard)
{
    if (nullptr == data || 0 == len)
    {
        return;
    }
    size_t payload_max = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        payload_max =
            is_fec_shard ? stream_max_fec_shard(static_cast<size_t>(max_datagram))
                         : stream_max_raw_sdu(static_cast<size_t>(max_datagram));
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
            queue_bytes -= wire_packet_bytes(queue.front());
            queue.pop_front();
            evicted = true;
        }
    }

    data_packet copy;
    shared_sized_buffer buf = pool.acquire(wire_len);
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
    auto sd = std::make_unique<sock_data>();
    sd->pts = 0;
    sd->buf = std::move(buf);
    copy.reset(std::move(sd));

    {
        std::lock_guard<std::mutex> lock(q_mu);
        queue_bytes += wire_len;
        queue.push_back(std::move(copy));
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
    for (auto &pkt : *air)
    {
        enqueue_wire_copy(pkt.data(), pkt.size(), true);
    }
}

void stream_sender::pace_wire_send(size_t bytes)
{
    const int cap_kbps = max_wire_kbps.load(std::memory_order_relaxed);
    if (cap_kbps <= 0)
    {
        return;
    }

    const double max_bps = static_cast<double>(cap_kbps) * 1000.0;
    const double max_bytes_per_sec = max_bps / 8.0;
    const double burst_bytes = max_bytes_per_sec * 0.25;

    while (!send_stop.load(std::memory_order_relaxed))
    {
        const double now = now_sec();
        if (pace_last_sec <= 0.)
        {
            pace_last_sec = now;
            pace_bucket_bytes = burst_bytes;
        }
        const double dt = now - pace_last_sec;
        pace_last_sec = now;
        pace_bucket_bytes += dt * max_bytes_per_sec;
        if (pace_bucket_bytes > burst_bytes)
        {
            pace_bucket_bytes = burst_bytes;
        }
        if (pace_bucket_bytes >= static_cast<double>(bytes))
        {
            pace_bucket_bytes -= static_cast<double>(bytes);
            return;
        }
        const double deficit = static_cast<double>(bytes) - pace_bucket_bytes;
        pace_bucket_bytes = 0.;
        const double sleep_s = deficit / max_bytes_per_sec;
        if (sleep_s > 0.)
        {
            std::this_thread::sleep_for(
                std::chrono::duration<double>(std::min(sleep_s, 0.05)));
        }
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

void stream_sender::stop_telemetry_thread()
{
    if (telemetry_thread.joinable())
    {
        telemetry_stop = true;
        telemetry_thread.join();
        telemetry_stop = false;
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

void stream_sender::telemetry_thread_main()
{
    uint8_t buf[512];
    while (!telemetry_stop.load(std::memory_order_relaxed))
    {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mu);
            fd = send_fd;
        }
        if (fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, 50);
        if (telemetry_stop.load(std::memory_order_relaxed))
        {
            break;
        }
        if (pr <= 0)
        {
            continue;
        }

        const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC);
        if (n <= 0)
        {
            continue;
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
        if (pace_reset.exchange(false, std::memory_order_relaxed))
        {
            pace_bucket_bytes = 0.;
            pace_last_sec = 0.;
        }

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

        data_packet pkt;
        {
            std::unique_lock<std::mutex> lock(q_mu);
            q_cv.wait_for(lock, std::chrono::milliseconds(wait_ms),
                          [this] { return send_stop || !queue.empty(); });
            if (send_stop)
            {
                break;
            }
            if (queue.empty())
            {
                continue;
            }
            pkt = std::move(queue.front());
            queue_bytes -= wire_packet_bytes(pkt);
            queue.pop_front();
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

        const sock_data &sd = data_packet::cast<sock_data>(pkt);
        if (sd.buf.size() >= k_stream_header_len)
        {
            const uint16_t seq = stream_sequence.fetch_add(1, std::memory_order_relaxed);
            stream_header_stamp_sequence(sd.buf.u8(), seq);
        }
        pace_wire_send(sd.buf.size());
        const ssize_t n = sendto(send_fd, sd.buf.u8(), sd.buf.size(), 0,
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

    std::string local_copy;
    bool        telemetry = true;
    {
        std::lock_guard<std::mutex> lock(mu);
        local_copy = local_spec;
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
    {
        std::string local_host;
        int         local_port = 0;
        if (parse_local_spec(local_copy, &local_host, &local_port) < 0 ||
            resolve_ipv4_bind_addr(local_host.c_str(), true, &bind_addr.sin_addr) < 0)
        {
            ::close(fd);
            return -EINVAL;
        }
        bind_addr.sin_port = htons(static_cast<uint16_t>(local_port));
    }
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
        send_thread = std::thread(&stream_sender::send_thread_main, this);

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
        telemetry_stop = false;
        if (telemetry)
        {
            telemetry_thread = std::thread(&stream_sender::telemetry_thread_main, this);
        }

        std::fprintf(stderr, "stream_sender: udp://%s:%d mtu=%d\n", host, port, mtu_local);
    }
    return 0;
}

void stream_sender::close()
{
    telemetry_stop = true;
    stop_telemetry_thread();
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

int stream_sender::input(uint8_t port, const data_packet &in)
{
    if (0 != port)
    {
        return -EINVAL;
    }
    const sock_data &src = data_packet::cast<sock_data>(in);
    const size_t     ingress_bytes = src.buf.size();

    bool oversized = false;
    fec_mode_e mode = fec_mode_e::block;
    {
        std::lock_guard<std::mutex> lock(mu);
        mode = fec_mode;
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
        enqueue_wire_copy(src.buf.u8(), src.buf.size(), false);
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
            const uint64_t before = fec.oversized();
            fec.push_app(src.buf.u8(), src.buf.size(), &air);
            oversized = fec.oversized() != before;
        }
        if (!air.empty())
        {
            enqueue_fec_air(&air);
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
    if ("local" == key)
    {
        std::string host;
        int         port = 0;
        if (parse_local_spec(value, &host, &port) < 0)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return -EBUSY;
        }
        local_spec.assign(value.data(), value.size());
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
    if ("max_datagram" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 64 || v > 65507 || value.size() > 31)
        {
            return -EINVAL;
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            max_datagram = static_cast<int>(v);
        }
        return reinit_fec_if_active();
    }
    if ("max_kbps" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 0 || v > 500'000 || value.size() > 31)
        {
            return -EINVAL;
        }
        max_wire_kbps.store(static_cast<int>(v), std::memory_order_relaxed);
        pace_reset.store(true, std::memory_order_relaxed);
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

    if ("stream" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = stream_spec;
        return 0;
    }
    if ("local" == key)
    {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mu);
            fd = send_fd;
        }
        if (fd < 0)
        {
            std::lock_guard<std::mutex> lock(mu);
            *value = local_spec;
            return 0;
        }
        sockaddr_in bound {};
        socklen_t   len = sizeof(bound);
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) < 0)
        {
            return -errno;
        }
        char host[INET_ADDRSTRLEN];
        const char *hip = inet_ntop(AF_INET, &bound.sin_addr, host, sizeof(host));
        if (nullptr == hip)
        {
            return -EINVAL;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s:%u", hip,
                      static_cast<unsigned>(ntohs(bound.sin_port)));
        *value = buf;
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
        std::lock_guard<std::mutex> lock(mu);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", max_datagram);
        *value = buf;
        return 0;
    }
    if ("max_input" == key)
    {
        fec_mode_e mode = fec_mode_e::block;
        int        dg = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            mode = fec_mode;
            dg = max_datagram;
        }
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
    if ("dropped" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, dropped.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
