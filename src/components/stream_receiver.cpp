#include "components/stream_receiver.hpp"

#include "core/host_util.hpp"
#include "core/key_util.hpp"
#include "core/port_caps.hpp"
#include "core/sequence_gap.hpp"
#include "core/stream_header.hpp"
#include "core/stream_telemetry.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <chrono>
#include <random>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

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

void note_fec_output_gaps(vstreamer::rs_block_erasure &fec, std::atomic<uint64_t> &fec_gap_count)
{
    const uint64_t n = fec.take_fail_lost_app_pkts();
    if (n > 0)
    {
        fec_gap_count.fetch_add(n, std::memory_order_relaxed);
    }
    (void)fec.take_fail_missing_shards();
    (void)fec.take_decode_fail();
}

}  // namespace

namespace vstreamer
{

namespace
{

size_t max_decoded_app_bytes_from_shard(size_t shard_max)
{
    if (shard_max <= rs_block_erasure::k_header_len + rs_block_erasure::k_len_prefix)
    {
        return 0;
    }
    return shard_max - rs_block_erasure::k_header_len - rs_block_erasure::k_len_prefix;
}

uint64_t monotonic_timestamp_us()
{
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

}  // namespace

const std::vector<port_desc> &stream_receiver::output_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::STREAM_DGRAM;
        p.caps.push_back(caps);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

stream_receiver::stream_receiver() : pool(static_cast<size_t>(1500), k_queue_depth) {}

stream_receiver::~stream_receiver()
{
    close();
}

std::string stream_receiver::name() const
{
    return "stream_receiver";
}

media_kind_e stream_receiver::output_kind() const
{
    return media_kind_e::UNKNOWN;
}

packet_kind_e stream_receiver::output_packet_kind(uint8_t port) const
{
    if (0 != port)
    {
        return packet_kind_e::UNKNOWN;
    }
    return packet_kind_e::SOCK;
}

void stream_receiver::enqueue_payload_buffer(shared_sized_buffer &&payload,
                                             size_t max_app_bytes, bool discont)
{
    if (payload.empty() || 0 == max_app_bytes || payload.size() > max_app_bytes)
    {
        std::lock_guard<std::mutex> lock(mu);
        recv_dropped++;
        return;
    }

    const size_t payload_len = payload.size();
    component_pdu pdu;
    pdu.ts_us = monotonic_timestamp_us();
    pdu.sdu_type = sdu_type_e::STREAM_DGRAM;
    pdu.port = 0;
    pdu.flags = 0;
    if (discont)
    {
        pdu.flags |= static_cast<uint8_t>(pdu_flag_e::DISCONT);
    }
    pdu.sdu = std::move(payload);

    bool evicted = false;
    {
        std::lock_guard<std::mutex> lock(q_mu);
        if (pdu_queue.size() >= k_queue_depth)
        {
            pdu_queue.pop_front();
            evicted = true;
        }
        pdu_queue.push_back(std::move(pdu));
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        fec_packet_received.fetch_add(1, std::memory_order_relaxed);
        recv_bytes.fetch_add(payload_len, std::memory_order_relaxed);
        if (evicted)
        {
            recv_dropped++;
        }
    }
    notify_wakeup();
    q_cv.notify_all();
}

void stream_receiver::enqueue_payloads(fec_rx_payload_list *payloads, size_t max_app_bytes)
{
    if (nullptr == payloads)
    {
        return;
    }
    for (auto &payload : *payloads)
    {
        bool discont = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (discont_next_)
            {
                discont = true;
                discont_next_ = false;
            }
        }
        enqueue_payload_buffer(std::move(payload), max_app_bytes, discont);
    }
    payloads->clear();
}

bool stream_receiver::ingest_datagram(const uint8_t *data, size_t len)
{
    fec_rx_payload_list payloads;
    bool                valid_media = false;
    size_t              enqueue_app_max = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (len > static_cast<size_t>(max_datagram))
        {
            rx_oversize.fetch_add(1, std::memory_order_relaxed);
            recv_dropped++;
            return false;
        }

        stream_header hdr {};
        const uint8_t *payload = nullptr;
        size_t         payload_len = 0;
        if (stream_header_parse(data, len, &hdr, &payload, &payload_len) < 0)
        {
            recv_dropped++;
            rx_bad_header++;
            return false;
        }

        if (!hdr.is_stream_data)
        {
            recv_dropped++;
            rx_bad_header++;
            return false;
        }

        const uint64_t dg = note_u16_forward_gap(hdr.sequence_number, last_udp_seq, have_udp_seq);
        if (dg > 0)
        {
            udp_gap_count.fetch_add(dg, std::memory_order_relaxed);
            discont_next_ = true;
        }
        udp_packet_received.fetch_add(1, std::memory_order_relaxed);
        recv_wire_bytes.fetch_add(len, std::memory_order_relaxed);

        const size_t shard_max = cached_max_fec_shard;

        if (hdr.is_fec)
        {
            if (payload_len < rs_block_erasure::k_header_len || 0 == shard_max ||
                payload_len > shard_max)
            {
                recv_dropped++;
                rx_bad_header++;
                return false;
            }
            shared_sized_buffer shard = pool.acquire(payload_len);
            if (0 == shard.capacity() || shard.size() != payload_len)
            {
                recv_dropped++;
                return false;
            }
            std::memcpy(shard.u8(), payload, payload_len);
            const uint64_t hdr_errors_before = fec.hdr_errors();
            fec.push_air(std::move(shard), &payloads);
            note_fec_output_gaps(fec, fec_gap_count);
            fec_rec = fec.recovered();
            fec_lost = fec.decode_fail();
            valid_media = fec.hdr_errors() == hdr_errors_before;
            enqueue_app_max = cached_max_decoded_app;
        }
        else
        {
            /* Raw path (fec none): one SDU per datagram; wire loss is payload loss too. */
            if (0 == cached_max_raw_sdu || payload_len > cached_max_raw_sdu)
            {
                recv_dropped++;
                return false;
            }
            shared_sized_buffer sdu = pool.acquire(payload_len);
            if (0 == sdu.capacity() || sdu.size() != payload_len)
            {
                recv_dropped++;
                return false;
            }
            std::memcpy(sdu.u8(), payload, payload_len);
            payloads.push_back(std::move(sdu));
            if (dg > 0)
            {
                /* Without FEC, a UDP gap is an SDU gap (mode switches may miscount slightly). */
                fec_gap_count.fetch_add(dg, std::memory_order_relaxed);
            }
            valid_media = true;
            enqueue_app_max = cached_max_raw_sdu;
        }
    }
    if (!payloads.empty() && enqueue_app_max > 0)
    {
        enqueue_payloads(&payloads, enqueue_app_max);
    }
    return valid_media;
}

void stream_receiver::stop_recv_thread()
{
    if (!recv_thread.joinable())
    {
        return;
    }
    recv_stop = true;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (recv_fd >= 0)
        {
            ::shutdown(recv_fd, SHUT_RDWR);
        }
    }
    q_cv.notify_all();
    recv_thread.join();
    {
        std::lock_guard<std::mutex> lock(mu);
        if (recv_fd >= 0)
        {
            ::close(recv_fd);
            recv_fd = -1;
        }
    }
    recv_stop = false;
}

void stream_receiver::recv_thread_main()
{
    /* Bounded wait so block expiry and the in-order emit queue advance even
     * when the wire goes quiet (e.g. tail of a burst loss). */
    constexpr int k_poll_ms = 10;
    uint8_t       buf[65536];
    sockaddr_in   peer_addr {};
    socklen_t     peer_len = 0;
    bool          have_peer = false;
    auto          next_report = std::chrono::steady_clock::now();
    while (!recv_stop)
    {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mu);
            fd = recv_fd;
        }
        if (fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, k_poll_ms);
        if (recv_stop)
        {
            break;
        }
        if (pr < 0)
        {
            if (EINTR == errno)
            {
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (pr > 0)
        {
            /* Drain everything that is ready before ticking. */
            for (;;)
            {
                sockaddr_in src {};
                msghdr      msg {};
                iovec       iov {};
                iov.iov_base = buf;
                iov.iov_len = sizeof(buf);
                msg.msg_name = &src;
                msg.msg_namelen = sizeof(src);
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                const ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_TRUNC);
                if (n <= 0)
                {
                    break;
                }
                if ((msg.msg_flags & MSG_TRUNC) != 0)
                {
                    rx_truncated.fetch_add(1, std::memory_order_relaxed);
                }
                if (ingest_datagram(buf, static_cast<size_t>(n)))
                {
                    const bool peer_changed =
                        have_peer && (0 != std::memcmp(&src, &peer_addr, sizeof(peer_addr)));
                    if (!have_peer || peer_changed)
                    {
                        if (peer_changed)
                        {
                            telemetry_peer_changes.fetch_add(1, std::memory_order_relaxed);
                        }
                        peer_addr = src;
                        peer_len = sizeof(peer_addr);
                        have_peer = true;
                        char       host[INET_ADDRSTRLEN];
                        const char *hip =
                            inet_ntop(AF_INET, &peer_addr.sin_addr, host, sizeof(host));
                        if (nullptr != hip)
                        {
                            char peer_str[64];
                            std::snprintf(peer_str, sizeof(peer_str), "%s:%u", hip,
                                          static_cast<unsigned>(ntohs(peer_addr.sin_port)));
                            std::lock_guard<std::mutex> plock(peer_display_mu);
                            telemetry_peer_str = peer_str;
                        }
                    }
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (have_peer && telemetry_on.load(std::memory_order_relaxed) && now >= next_report)
        {
            const int interval = telemetry_ms.load(std::memory_order_relaxed);
            stream_link_report rep {};
            rep.session_id = session_id.load(std::memory_order_relaxed);
            rep.timestamp_us = monotonic_timestamp_us();
            rep.counters = link_counters_snapshot();
            const uint16_t seq = static_cast<uint16_t>(report_seq.fetch_add(1, std::memory_order_relaxed));
            rep.report_seq = seq;

            uint8_t wire[k_stream_header_len + k_stream_link_report_payload_len];
            stream_header thdr {};
            thdr.sequence_number = seq;
            thdr.is_fec = false;
            thdr.is_stream_data = false;
            thdr.ext_len = 0;
            stream_header_write(wire, thdr);
            stream_link_report_encode_payload(rep, wire + k_stream_header_len,
                                              k_stream_link_report_payload_len);
            const ssize_t sent =
                sendto(fd, wire, sizeof(wire), MSG_DONTWAIT,
                       reinterpret_cast<sockaddr *>(&peer_addr), peer_len);
            if (sent == static_cast<ssize_t>(sizeof(wire)))
            {
                telemetry_sent.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                telemetry_send_errors.fetch_add(1, std::memory_order_relaxed);
            }
            /* Advance by one interval; never burst-send to catch up. */
            next_report += std::chrono::milliseconds(interval);
            if (now - next_report > std::chrono::milliseconds(interval))
            {
                next_report = now + std::chrono::milliseconds(interval);
            }
        }

        fec_rx_payload_list payloads;
        size_t              decoded_max = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            fec.poll_rx(&payloads);
            note_fec_output_gaps(fec, fec_gap_count);
            fec_lost = fec.decode_fail();
            decoded_max = cached_max_decoded_app;
        }
        if (!payloads.empty() && decoded_max > 0)
        {
            enqueue_payloads(&payloads, decoded_max);
        }
    }
}

stream_link_counters stream_receiver::link_counters_snapshot() const
{
    stream_link_counters c;
    c.udp_packet_received = udp_packet_received.load(std::memory_order_relaxed);
    c.fec_packet_received = fec_packet_received.load(std::memory_order_relaxed);
    c.udp_gap_count = udp_gap_count.load(std::memory_order_relaxed);
    c.fec_gap_count = fec_gap_count.load(std::memory_order_relaxed);
    return c;
}

int stream_receiver::open()
{
    std::string spec_copy;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return 0;
        }
        if (listen_spec.empty())
        {
            return -EINVAL;
        }
        spec_copy = listen_spec;
    }

    char host[128];
    int  port = 0;
    if (parse_host_port(spec_copy, host, sizeof(host), &port) < 0)
    {
        if (spec_copy.size() > 0 && spec_copy[0] == ':')
        {
            std::string p = "0";
            p += spec_copy;
            if (parse_host_port(p, host, sizeof(host), &port) < 0)
            {
                return -EINVAL;
            }
        }
        else
        {
            return -EINVAL;
        }
    }

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -errno;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (resolve_ipv4_bind_addr(host, true, &addr.sin_addr) < 0)
    {
        ::close(fd);
        return -EINVAL;
    }

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        const int err = errno;
        std::fprintf(stderr, "stream_receiver: bind %s:%d failed: %s\n", host, port,
                     std::strerror(err));
        ::close(fd);
        return -err;
    }

    constexpr int k_sock_buf = 16 * 1024 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &k_sock_buf, sizeof(k_sock_buf));

    uint32_t new_session = 0;
    {
        std::random_device rd;
        for (int attempt = 0; attempt < 16 && 0 == new_session; ++attempt)
        {
            new_session = static_cast<uint32_t>(rd());
        }
        if (0 == new_session)
        {
            new_session = 1;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            ::close(fd);
            return 0;
        }
        recv_fd = fd;
        session_id.store(new_session, std::memory_order_relaxed);
        report_seq.store(0, std::memory_order_relaxed);
        telemetry_sent.store(0, std::memory_order_relaxed);
        telemetry_send_errors.store(0, std::memory_order_relaxed);
        telemetry_peer_changes.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> plock(peer_display_mu);
            telemetry_peer_str.clear();
        }
        udp_packet_received.store(0, std::memory_order_relaxed);
        fec_packet_received.store(0, std::memory_order_relaxed);
        udp_gap_count.store(0, std::memory_order_relaxed);
        fec_gap_count.store(0, std::memory_order_relaxed);
        last_udp_seq = 0;
        have_udp_seq = false;
        discont_next_ = false;
        rx_bad_header = 0;
        cached_max_fec_shard = stream_max_fec_shard(static_cast<size_t>(max_datagram));
        cached_max_decoded_app = max_decoded_app_bytes_from_shard(cached_max_fec_shard);
        cached_max_raw_sdu = stream_max_raw_sdu(static_cast<size_t>(max_datagram));
        rx_oversize.store(0, std::memory_order_relaxed);
        opened = true;
        recv_stop = false;
        recv_thread = std::thread(&stream_receiver::recv_thread_main, this);
    }

    {
        std::lock_guard<std::mutex> qlock(q_mu);
        stopping = false;
    }

    std::fprintf(stderr, "stream_receiver: listen %s:%d\n", host, port);
    return 0;
}

void stream_receiver::close()
{
    {
        std::lock_guard<std::mutex> qlock(q_mu);
        stopping = true;
    }
    q_cv.notify_all();

    stop_recv_thread();

    std::lock_guard<std::mutex> lock(mu);
    opened = false;
    if (recv_fd >= 0)
    {
        ::close(recv_fd);
        recv_fd = -1;
    }

    {
        std::lock_guard<std::mutex> qlock(q_mu);
        while (!pdu_queue.empty())
        {
            pdu_queue.pop_front();
        }
    }
    q_cv.notify_all();
}

int stream_receiver::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(q_mu);
    if (pdu_queue.empty())
    {
        return -EAGAIN;
    }
    out = std::move(pdu_queue.front());
    pdu_queue.pop_front();
    return 0;
}

int stream_receiver::output(uint8_t port, data_packet &out, int timeout_ms)
{
    if (0 != port)
    {
        return -EINVAL;
    }

    std::unique_lock<std::mutex> lock(q_mu);
    if (pdu_queue.empty() && 0 != timeout_ms)
    {
        if (timeout_ms < 0)
        {
            q_cv.wait(lock, [this] { return stopping || !pdu_queue.empty(); });
        }
        else
        {
            q_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return stopping || !pdu_queue.empty(); });
        }
    }

    if (pdu_queue.empty())
    {
        if (stopping)
        {
            return -EBADF;
        }
        return -EAGAIN;
    }

    component_pdu pdu = std::move(pdu_queue.front());
    pdu_queue.pop_front();
    const size_t egress_bytes = pdu.sdu.size();
    lock.unlock();
    {
        std::lock_guard<std::mutex> slock(mu);
        update_kbps_window(egress_rate_t0, egress_rate_bytes, egress_kbps, egress_bytes);
    }
    auto sd = std::make_unique<sock_data>();
    sd->pts = static_cast<int64_t>(pdu.ts_us);
    sd->buf = std::move(pdu.sdu);
    out.reset(std::move(sd));
    return 0;
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

int stream_receiver::configure(std::string_view key, std::string_view value)
{
    if ("telemetry" == key)
    {
        bool on = false;
        if (!parse_on_off(value, &on))
        {
            return -EINVAL;
        }
        telemetry_on.store(on, std::memory_order_relaxed);
        return 0;
    }
    if ("telemetry_ms" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 20 || v > 5000 || value.size() > 31)
        {
            return -EINVAL;
        }
        telemetry_ms.store(static_cast<int>(v), std::memory_order_relaxed);
        return 0;
    }
    if ("listen" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        listen_spec.assign(value.data(), value.size());
        return 0;
    }
    if ("max_datagram" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) < 0 || v < 64 || v > 65507 || value.size() > 31)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return -EBUSY;
        }
        max_datagram = static_cast<int>(v);
        return 0;
    }
    return -ENOTSUP;
}

int stream_receiver::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    int r = port_caps_query(output_ports(), false, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }
    if ("listen" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = listen_spec;
        return 0;
    }
    if ("out_rate" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(egress_kbps));
        *value = buf;
        return 0;
    }
    if ("udp_packet_received" == key || "fec_packet_received" == key || "udp_gap_count" == key ||
        "fec_gap_count" == key)
    {
        uint64_t n = 0;
        if ("udp_packet_received" == key)
        {
            n = udp_packet_received.load(std::memory_order_relaxed);
        }
        else if ("fec_packet_received" == key)
        {
            n = fec_packet_received.load(std::memory_order_relaxed);
        }
        else if ("udp_gap_count" == key)
        {
            n = udp_gap_count.load(std::memory_order_relaxed);
        }
        else
        {
            n = fec_gap_count.load(std::memory_order_relaxed);
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, n);
        *value = buf;
        return 0;
    }
    if ("fec_recovered" == key || "fec_failures" == key || "fec_hdr_errors" == key ||
        "fec_kn_mismatch" == key || "fec_evicted_blocks" == key || "fec_rs_failures" == key ||
        "fec_missing_shards" == key || "fec_late_blocks" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char           buf[32];
        uint64_t       n = 0;
        if ("fec_recovered" == key)
        {
            n = fec_rec;
        }
        else if ("fec_failures" == key)
        {
            n = fec_lost;
        }
        else if ("fec_hdr_errors" == key)
        {
            n = fec.hdr_errors();
        }
        else if ("fec_kn_mismatch" == key)
        {
            n = fec.kn_mismatch();
        }
        else if ("fec_evicted_blocks" == key)
        {
            n = fec.evicted_blocks();
        }
        else if ("fec_rs_failures" == key)
        {
            n = fec.rs_failures();
        }
        else if ("fec_missing_shards" == key)
        {
            n = fec.missing_shards();
        }
        else
        {
            n = fec.late_blocks();
        }
        std::snprintf(buf, sizeof(buf), "%" PRIu64, n);
        *value = buf;
        return 0;
    }
    if ("telemetry" == key)
    {
        *value = telemetry_on.load(std::memory_order_relaxed) ? "on" : "off";
        return 0;
    }
    if ("telemetry_ms" == key)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", telemetry_ms.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    if ("telemetry_sent" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64,
                      telemetry_sent.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    if ("telemetry_send_errors" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64,
                      telemetry_send_errors.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    if ("telemetry_peer_changes" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64,
                      telemetry_peer_changes.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    if ("session_id" == key)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%" PRIu32, session_id.load(std::memory_order_relaxed));
        *value = buf;
        return 0;
    }
    if ("telemetry_peer" == key)
    {
        std::lock_guard<std::mutex> plock(peer_display_mu);
        *value = telemetry_peer_str;
        return 0;
    }
    if ("stats" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[420];
        std::snprintf(buf, sizeof(buf),
                      "udp_packet_received=%" PRIu64 " fec_packet_received=%" PRIu64
                      " udp_gap_count=%" PRIu64 " fec_gap_count=%" PRIu64
                      " telemetry_sent=%" PRIu64 " telemetry_send_errors=%" PRIu64
                      " bytes=%" PRIu64 " wire_bytes=%" PRIu64 " dropped=%" PRIu64
                      " out_rate=%.1f fec_recovered=%" PRIu64 " fec_failures=%" PRIu64,
                      udp_packet_received.load(std::memory_order_relaxed),
                      fec_packet_received.load(std::memory_order_relaxed),
                      udp_gap_count.load(std::memory_order_relaxed),
                      fec_gap_count.load(std::memory_order_relaxed),
                      telemetry_sent.load(std::memory_order_relaxed),
                      telemetry_send_errors.load(std::memory_order_relaxed),
                      recv_bytes.load(std::memory_order_relaxed),
                      recv_wire_bytes.load(std::memory_order_relaxed), recv_dropped,
                      static_cast<double>(egress_kbps), fec_rec,
                      fec_lost);
        *value = buf;
        return 0;
    }
    if ("rx_truncated" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64,
                      rx_truncated.load(std::memory_order_relaxed));
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
    if ("rx_bad_header" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, rx_bad_header);
        *value = buf;
        return 0;
    }
    if ("rx_oversize" == key)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, rx_oversize.load(std::memory_order_relaxed));
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
    return -ENOTSUP;
}

}  // namespace vstreamer
