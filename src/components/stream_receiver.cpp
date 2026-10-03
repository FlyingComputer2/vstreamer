#include "components/stream_receiver.hpp"

#include "core/host_util.hpp"
#include "core/key_util.hpp"
#include "core/sequence_gap.hpp"
#include "core/stream_header.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <chrono>
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

size_t max_fec_shard_bytes(int max_datagram)
{
    if (max_datagram <= static_cast<int>(k_stream_header_len))
    {
        return 0;
    }
    return static_cast<size_t>(max_datagram) - k_stream_header_len;
}

size_t max_decoded_app_bytes(int max_datagram)
{
    const size_t shard = max_fec_shard_bytes(max_datagram);
    if (shard <= rs_block_erasure::k_header_len + rs_block_erasure::k_len_prefix)
    {
        return 0;
    }
    return shard - rs_block_erasure::k_header_len - rs_block_erasure::k_len_prefix;
}

}  // namespace

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

void stream_receiver::enqueue_payload_buffer(shared_sized_buffer &&payload)
{
    int dg = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        dg = max_datagram;
    }
    const size_t app_max = max_decoded_app_bytes(dg);
    if (payload.empty() || 0 == app_max || payload.size() > app_max)
    {
        std::lock_guard<std::mutex> lock(mu);
        recv_dropped++;
        return;
    }

    const size_t payload_len = payload.size();
    data_packet  pkt;
    auto         sd = std::make_unique<sock_data>();
    sd->pts = 0;
    sd->seq = fec_payload_sequence++;
    sd->buf = std::move(payload);
    pkt.reset(std::move(sd));

    bool evicted = false;
    {
        std::lock_guard<std::mutex> lock(q_mu);
        if (payload_queue.size() >= k_queue_depth)
        {
            payload_queue.pop_front();
            evicted = true;
        }
        payload_queue.push_back(std::move(pkt));
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
    q_cv.notify_all();
}

void stream_receiver::enqueue_payloads(fec_rx_payload_list *payloads)
{
    if (nullptr == payloads)
    {
        return;
    }
    for (auto &payload : *payloads)
    {
        enqueue_payload_buffer(std::move(payload));
    }
    payloads->clear();
}

void stream_receiver::ingest_datagram(const uint8_t *data, size_t len)
{
    fec_rx_payload_list payloads;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (len > static_cast<size_t>(max_datagram))
        {
            rx_oversize.fetch_add(1, std::memory_order_relaxed);
            recv_dropped++;
            return;
        }
        udp_packet_received.fetch_add(1, std::memory_order_relaxed);
        recv_wire_bytes.fetch_add(len, std::memory_order_relaxed);

        const size_t shard_max = max_fec_shard_bytes(max_datagram);

        /* stream_header_s always prefixes the FEC shard (UDP gap telemetry). */
        const uint8_t *fec_buf = data;
        size_t         fec_len = len;
        if (stream_datagram_len_ok(len))
        {
            const uint16_t seq = stream_header_sequence_be16(data);
            const uint64_t dg = note_u16_forward_gap(seq, last_udp_seq, have_udp_seq);
            if (dg > 0)
            {
                udp_gap_count.fetch_add(dg, std::memory_order_relaxed);
            }
            const uint8_t *fec_ptr = stream_fec_shard(data, len, &fec_len);
            if (fec_ptr != nullptr)
            {
                fec_buf = fec_ptr;
            }
        }

        if (fec_len < rs_block_erasure::k_header_len || 0 == shard_max || fec_len > shard_max)
        {
            recv_dropped++;
            return;
        }
        shared_sized_buffer shard = pool.acquire(fec_len);
        if (0 == shard.capacity() || shard.size() != fec_len)
        {
            recv_dropped++;
            return;
        }
        std::memcpy(shard.u8(), fec_buf, fec_len);
        fec.push_air(std::move(shard), &payloads);
        note_fec_output_gaps(fec, fec_gap_count);
        fec_rec = fec.recovered();
        fec_lost = fec.decode_fail();
    }
    enqueue_payloads(&payloads);
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
                msghdr msg {};
                iovec  iov {};
                iov.iov_base = buf;
                iov.iov_len = sizeof(buf);
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
                ingest_datagram(buf, static_cast<size_t>(n));
            }
        }

        fec_rx_payload_list payloads;
        {
            std::lock_guard<std::mutex> lock(mu);
            fec.poll_rx(&payloads);
            note_fec_output_gaps(fec, fec_gap_count);
            fec_lost = fec.decode_fail();
        }
        enqueue_payloads(&payloads);
    }
}

stream_receiver_counters stream_receiver::link_counters_snapshot() const
{
    stream_receiver_counters c;
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

    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            ::close(fd);
            return 0;
        }
        recv_fd = fd;
        udp_packet_received.store(0, std::memory_order_relaxed);
        fec_packet_received.store(0, std::memory_order_relaxed);
        udp_gap_count.store(0, std::memory_order_relaxed);
        fec_gap_count.store(0, std::memory_order_relaxed);
        last_udp_seq = 0;
        have_udp_seq = false;
        fec_payload_sequence = 0;
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
        while (!payload_queue.empty())
        {
            payload_queue.pop_front();
        }
    }
    q_cv.notify_all();
}

int stream_receiver::output(uint8_t port, data_packet &out, int timeout_ms)
{
    if (0 != port)
    {
        return -EINVAL;
    }

    std::unique_lock<std::mutex> lock(q_mu);
    if (payload_queue.empty() && timeout_ms != 0)
    {
        if (timeout_ms < 0)
        {
            q_cv.wait(lock, [this] { return stopping || !payload_queue.empty(); });
        }
        else
        {
            q_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return stopping || !payload_queue.empty(); });
        }
    }

    if (payload_queue.empty())
    {
        if (stopping)
        {
            return -EBADF;
        }
        return -EAGAIN;
    }

    const size_t egress_bytes =
        data_packet::cast<sock_data>(payload_queue.front()).buf.size();
    out = std::move(payload_queue.front());
    payload_queue.pop_front();
    lock.unlock();
    {
        std::lock_guard<std::mutex> slock(mu);
        update_kbps_window(egress_rate_t0, egress_rate_bytes, egress_kbps, egress_bytes);
    }
    return 0;
}

int stream_receiver::configure(std::string_view key, std::string_view value)
{
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
    if ("stats" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "udp_packet_received=%" PRIu64 " fec_packet_received=%" PRIu64
                      " udp_gap_count=%" PRIu64 " fec_gap_count=%" PRIu64
                      " bytes=%" PRIu64 " wire_bytes=%" PRIu64 " dropped=%" PRIu64
                      " out_rate=%.1f fec_recovered=%" PRIu64 " fec_failures=%" PRIu64,
                      udp_packet_received.load(std::memory_order_relaxed),
                      fec_packet_received.load(std::memory_order_relaxed),
                      udp_gap_count.load(std::memory_order_relaxed),
                      fec_gap_count.load(std::memory_order_relaxed),
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
