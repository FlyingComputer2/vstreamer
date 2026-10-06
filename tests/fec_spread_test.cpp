#include "components/stream_receiver.hpp"
#include "components/stream_sender.hpp"
#include "core/data_packet.hpp"
#include "core/fec_spread.hpp"
#include "core/packet_types.hpp"
#include "core/rs_block_erasure.hpp"
#include "core/shared_sized_buffer.hpp"
#include "core/stream_header.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using vstreamer::rs_block_erasure;
using clock_type = std::chrono::steady_clock;

namespace
{

int cfg(vstreamer::component &c, std::string_view key, std::string_view value)
{
    return c.configure(key, value);
}

vstreamer::data_packet app_packet(uint8_t tag, size_t len = 64)
{
    std::vector<uint8_t> storage(len, tag);
    auto sd = std::make_unique<vstreamer::sock_data>();
    sd->buf = vstreamer::shared_sized_buffer::copy_from(storage.data(), storage.size());
    vstreamer::data_packet pkt;
    pkt.reset(std::move(sd));
    return pkt;
}

/* A loopback UDP socket standing in for the receiver; records arrival time and FEC header. */
class wire_sniffer
{
public:
    struct shard
    {
        clock_type::time_point at;
        uint16_t               wire_seq = 0;
        uint16_t               sdu_base = 0;
        int                    idx = -1;
        int                    n_sent = 0;
    };

    wire_sniffer()
    {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(0, bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
        socklen_t len = sizeof(addr);
        getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }
    ~wire_sniffer()
    {
        close(fd_);
    }

    std::string host_port() const
    {
        return "127.0.0.1:" + std::to_string(port_);
    }

    /* Collects datagrams until count arrive or timeout_ms passes. */
    std::vector<shard> collect(size_t count, int timeout_ms)
    {
        std::vector<shard> out;
        const auto         deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);
        uint8_t            buf[2048];
        while (out.size() < count && clock_type::now() < deadline)
        {
            pollfd pfd {fd_, POLLIN, 0};
            if (poll(&pfd, 1, 5) <= 0)
            {
                continue;
            }
            const ssize_t n = recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < static_cast<ssize_t>(vstreamer::k_stream_header_len +
                                         rs_block_erasure::k_header_len))
            {
                continue;
            }
            shard s;
            s.at = clock_type::now();
            s.wire_seq = vstreamer::stream_header_sequence_be16(buf);
            const uint8_t *fec = buf + vstreamer::k_stream_header_len;
            int            k = 0;
            int            n_total = 0;
            int            sdu_n = 0;
            EXPECT_TRUE(rs_block_erasure::unpack_header(
                fec, static_cast<size_t>(n) - vstreamer::k_stream_header_len, &s.sdu_base,
                &s.idx, &k, &n_total, &sdu_n));
            s.n_sent = sdu_n + (n_total - k);
            out.push_back(s);
        }
        return out;
    }

private:
    int      fd_ = -1;
    uint16_t port_ = 0;
};

double ms_between(clock_type::time_point a, clock_type::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

void open_fec_sender(vstreamer::stream_sender &sender, const std::string &dst, int k, int n,
                     int spread_ms)
{
    ASSERT_EQ(0, cfg(sender, "stream", dst));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    /* k first: the default n (12) must stay >= k at every step. */
    ASSERT_EQ(0, cfg(sender, "fec_k", std::to_string(k)));
    ASSERT_EQ(0, cfg(sender, "fec_n", std::to_string(n)));
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", std::to_string(spread_ms)));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));
}

std::vector<std::vector<uint8_t>> encode_tags(rs_block_erasure &enc, uint16_t sdu_base,
                                              const std::vector<uint8_t> &tags)
{
    std::vector<std::vector<uint8_t>> apps;
    for (uint8_t t : tags)
    {
        apps.push_back(std::vector<uint8_t>(32, t));
    }
    std::vector<std::vector<uint8_t>> air;
    EXPECT_TRUE(enc.encode_block(apps, sdu_base, &air));
    return air;
}

std::vector<uint8_t> decode_order(rs_block_erasure &dec,
                                  const std::vector<std::vector<uint8_t>> &wire,
                                  const std::vector<bool> &lost)
{
    vstreamer::fec_rx_payload_list out;
    for (size_t i = 0; i < wire.size(); i++)
    {
        if (lost[i])
        {
            continue;
        }
        vstreamer::fec_rx_payload_list step;
        dec.push_air(wire[i].data(), wire[i].size(), &step);
        for (auto &p : step)
        {
            out.push_back(std::move(p));
        }
    }
    vstreamer::fec_rx_payload_list tail;
    dec.poll_rx(&tail);
    for (auto &p : tail)
    {
        out.push_back(std::move(p));
    }
    std::vector<uint8_t> tags;
    for (const auto &p : out)
    {
        tags.push_back(p.u8()[0]);
    }
    return tags;
}

}  // namespace

TEST(FecSpreadTest, OffsetsTileTheWindow)
{
    using std::chrono::microseconds;
    EXPECT_EQ(vstreamer::fec_spread_offset(0, 24, 20), microseconds(0));
    EXPECT_EQ(vstreamer::fec_spread_offset(1, 24, 20), microseconds(833));
    EXPECT_EQ(vstreamer::fec_spread_offset(23, 24, 20), microseconds(19166));
    EXPECT_EQ(vstreamer::fec_spread_offset(8, 9, 20), microseconds(17777));
    EXPECT_EQ(vstreamer::fec_spread_offset(5, 24, 0), microseconds(0));
    EXPECT_EQ(vstreamer::fec_spread_offset(5, 0, 20), microseconds(0));
}

TEST(FecSpreadTest, ConfigRangeAndQuery)
{
    vstreamer::stream_sender sender;
    EXPECT_EQ(0, cfg(sender, "fec_spread_ms", "0"));
    EXPECT_EQ(0, cfg(sender, "fec_spread_ms", "40"));
    EXPECT_EQ(0, cfg(sender, "fec_spread_ms", "500"));
    EXPECT_EQ(-EINVAL, cfg(sender, "fec_spread_ms", "-1"));
    EXPECT_EQ(-EINVAL, cfg(sender, "fec_spread_ms", "x"));
    std::string v;
    ASSERT_EQ(0, sender.query("fec_spread_ms", &v));
    EXPECT_EQ("500", v);
}

/* spread 0 is today's behaviour: a whole block leaves back to back. */
TEST(FecSpreadTest, ZeroSendsBlockAtOnce)
{
    wire_sniffer             sniff;
    vstreamer::stream_sender sender;
    open_fec_sender(sender, sniff.host_port(), 4, 6, 0);
    for (uint8_t t = 0; t < 4; t++)
    {
        ASSERT_EQ(0, sender.input(0, app_packet(t)));
    }
    const auto got = sniff.collect(6, 1000);
    ASSERT_EQ(6u, got.size());
    EXPECT_LT(ms_between(got.front().at, got.back().at), 5.0);
    sender.close();
}

/* One block, spread 30 ms: shards leave in index order, about 30/6 = 5 ms apart. */
TEST(FecSpreadTest, BlockSpreadOverWindowInOrder)
{
    wire_sniffer             sniff;
    vstreamer::stream_sender sender;
    open_fec_sender(sender, sniff.host_port(), 4, 6, 30);
    for (uint8_t t = 0; t < 4; t++)
    {
        ASSERT_EQ(0, sender.input(0, app_packet(t)));
    }
    const auto got = sniff.collect(6, 1000);
    ASSERT_EQ(6u, got.size());
    for (size_t i = 0; i < got.size(); i++)
    {
        EXPECT_EQ(static_cast<int>(i), got[i].idx);
        const double expect_ms = 30.0 * static_cast<double>(i) / 6.0;
        EXPECT_NEAR(ms_between(got.front().at, got[i].at), expect_ms, 4.0) << "shard " << i;
    }
    sender.close();
}

/* A timeout-flushed block keeps full parity and spreads over the same window. */
TEST(FecSpreadTest, ShortBlockSpreadsFullParity)
{
    wire_sniffer             sniff;
    vstreamer::stream_sender sender;
    open_fec_sender(sender, sniff.host_port(), 8, 12, 20);
    ASSERT_EQ(0, sender.input(0, app_packet(1)));  // flushed by the 20 ms timeout
    const auto got = sniff.collect(5, 1000);
    ASSERT_EQ(5u, got.size());  // 1 data + 4 parity
    EXPECT_EQ(5, got.front().n_sent);
    EXPECT_NEAR(ms_between(got.front().at, got.back().at), 20.0 * 4 / 5, 4.0);
    sender.close();
}

/* Two blocks closed 10 ms apart with a 30 ms window overlap: their shards interleave on the
 * wire, and the wire sequence still counts up in send order. */
TEST(FecSpreadTest, OverlappingBlocksInterleave)
{
    wire_sniffer             sniff;
    vstreamer::stream_sender sender;
    open_fec_sender(sender, sniff.host_port(), 4, 6, 30);
    for (uint8_t t = 0; t < 4; t++)
    {
        ASSERT_EQ(0, sender.input(0, app_packet(t)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    for (uint8_t t = 4; t < 8; t++)
    {
        ASSERT_EQ(0, sender.input(0, app_packet(t)));
    }
    const auto got = sniff.collect(12, 1000);
    ASSERT_EQ(12u, got.size());
    const uint16_t first_base = got.front().sdu_base;
    size_t         last_a = 0;
    size_t         first_b = got.size();
    for (size_t i = 0; i < got.size(); i++)
    {
        if (got[i].sdu_base == first_base)
        {
            last_a = i;
        }
        else if (first_b == got.size())
        {
            first_b = i;
        }
        if (i > 0)
        {
            EXPECT_EQ(static_cast<uint16_t>(got[i - 1].wire_seq + 1), got[i].wire_seq);
        }
    }
    EXPECT_LT(first_b, last_a) << "block B should start before block A finishes";
    sender.close();
}

/* Raw (fec none) datagrams are never delayed by the spread setting. */
TEST(FecSpreadTest, RawNotDelayed)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(0, bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);

    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:" + std::to_string(ntohs(addr.sin_port))));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", "40"));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    const auto t0 = clock_type::now();
    ASSERT_EQ(0, sender.input(0, app_packet(7)));
    pollfd pfd {fd, POLLIN, 0};
    ASSERT_EQ(1, poll(&pfd, 1, 500));
    EXPECT_LT(ms_between(t0, clock_type::now()), 10.0);
    sender.close();
    close(fd);
}

/* Receiver in a running stream: shards of two blocks arriving interleaved decode in order,
 * with a loss in each (including the first shard of the earlier block). */
TEST(FecSpreadTest, ReceiverDecodesInterleavedBlocks)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    const auto prior = encode_tags(enc, 96, {90, 91, 92, 93});
    const auto a = encode_tags(enc, 100, {1, 2, 3, 4});
    const auto b = encode_tags(enc, 104, {5, 6, 7, 8});
    std::vector<std::vector<uint8_t>> wire;
    for (size_t i = 0; i < a.size(); i++)
    {
        wire.push_back(a[i]);
        wire.push_back(b[i]);
    }
    std::vector<bool> lost(wire.size(), false);
    lost[0] = true;  // A data 0
    lost[3] = true;  // B data 1
    lost[6] = true;  // A data 3
    rs_block_erasure dec;
    EXPECT_EQ((std::vector<uint8_t> {90, 91, 92, 93}),
              decode_order(dec, prior, std::vector<bool>(prior.size(), false)));
    EXPECT_EQ((std::vector<uint8_t> {1, 2, 3, 4, 5, 6, 7, 8}), decode_order(dec, wire, lost));
}

/* Start-up edge case, the same with or without spreading: a fresh receiver anchors in-order
 * delivery on the first shard it sees. If the earlier block's first shard is lost and a later
 * block's arrives first, the earlier block is treated as late and delivered after it. */
TEST(FecSpreadTest, FreshReceiverAnchorsOnFirstShardSeen)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    const auto a = encode_tags(enc, 100, {1, 2, 3, 4});
    const auto b = encode_tags(enc, 104, {5, 6, 7, 8});
    std::vector<std::vector<uint8_t>> wire;
    for (size_t i = 0; i < a.size(); i++)
    {
        wire.push_back(a[i]);
        wire.push_back(b[i]);
    }
    std::vector<bool> lost(wire.size(), false);
    lost[0] = true;  // A data 0: B data 0 is the first shard this receiver sees
    rs_block_erasure dec;
    EXPECT_EQ((std::vector<uint8_t> {5, 6, 7, 8, 1, 2, 3, 4}), decode_order(dec, wire, lost));
}

/* A burst of 4 consecutive wire losses: sent back to back it takes 4 shards of one 4/6 block
 * (unrecoverable); interleaved over two blocks it takes 2 of each (both recover). */
TEST(FecSpreadTest, InterleavingSurvivesBurst)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    const auto a = encode_tags(enc, 200, {1, 2, 3, 4});
    const auto b = encode_tags(enc, 204, {5, 6, 7, 8});

    std::vector<std::vector<uint8_t>> serial(a);
    serial.insert(serial.end(), b.begin(), b.end());
    std::vector<std::vector<uint8_t>> mixed;
    for (size_t i = 0; i < a.size(); i++)
    {
        mixed.push_back(a[i]);
        mixed.push_back(b[i]);
    }
    std::vector<bool> burst(12, false);
    for (size_t i = 1; i < 5; i++)
    {
        burst[i] = true;
    }

    rs_block_erasure dec_serial;
    const auto       got_serial = decode_order(dec_serial, serial, burst);
    EXPECT_LT(got_serial.size(), 8u);

    rs_block_erasure dec_mixed;
    EXPECT_EQ((std::vector<uint8_t> {1, 2, 3, 4, 5, 6, 7, 8}),
              decode_order(dec_mixed, mixed, burst));
}

/* End to end over loopback: sender with spreading, receiver decodes every packet in order. */
TEST(FecSpreadTest, LoopbackInOrderWithSpread)
{
    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    /* Pick a free port for the receiver to bind. */
    const int fd_port_probe = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(0, bind(fd_port_probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
    socklen_t len = sizeof(addr);
    getsockname(fd_port_probe, reinterpret_cast<sockaddr *>(&addr), &len);
    close(fd_port_probe);
    const std::string host_port = "127.0.0.1:" + std::to_string(ntohs(addr.sin_port));

    ASSERT_EQ(0, cfg(receiver, "listen", host_port));
    ASSERT_EQ(0, receiver.open());
    open_fec_sender(sender, host_port, 4, 6, 20);

    constexpr int k_packets = 40;
    for (int i = 0; i < k_packets; i++)
    {
        ASSERT_EQ(0, sender.input(0, app_packet(static_cast<uint8_t>(i))));
        if (i % 7 == 6)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
    std::vector<uint8_t> tags;
    vstreamer::data_packet out;
    const auto deadline = clock_type::now() + std::chrono::seconds(2);
    while (tags.size() < k_packets && clock_type::now() < deadline)
    {
        if (0 == receiver.output(0, out, 50))
        {
            const auto &sd = vstreamer::data_packet::cast<vstreamer::sock_data>(out);
            tags.push_back(sd.buf.u8()[0]);
        }
    }
    ASSERT_EQ(static_cast<size_t>(k_packets), tags.size());
    for (int i = 0; i < k_packets; i++)
    {
        EXPECT_EQ(static_cast<uint8_t>(i), tags[static_cast<size_t>(i)]);
    }
    sender.close();
    receiver.close();
}

/* The timeout flush runs on the send thread. A packet that opens a new block while the thread
 * sleeps (no other traffic) must still be flushed about timeout_ms later, not when the thread's
 * older, longer sleep ends. Repeated so the packet lands at different points of that sleep. */
TEST(FecSpreadTest, TimeoutFlushOnTimeWhenIdle)
{
    wire_sniffer             sniff;
    vstreamer::stream_sender sender;
    open_fec_sender(sender, sniff.host_port(), 8, 12, 0);
    double worst_ms = 0.;
    for (int i = 0; i < 12; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(7 * i % 50 + 60));
        const auto t0 = clock_type::now();
        ASSERT_EQ(0, sender.input(0, app_packet(static_cast<uint8_t>(i))));
        const auto got = sniff.collect(5, 500);  // 1 data + 4 parity
        ASSERT_EQ(5u, got.size());
        worst_ms = std::max(worst_ms, ms_between(t0, got.front().at));
    }
    EXPECT_LT(worst_ms, 30.0) << "timeout is 20 ms";
    sender.close();
}
