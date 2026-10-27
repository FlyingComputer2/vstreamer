#include "core/default_reactor_host.hpp"
#include "components/stream_receiver.hpp"
#include "components/stream_sender.hpp"
#include "core/fec_spread.hpp"
#include "core/rs_block_erasure.hpp"
#include <bfc/shared_sized_buffer.hpp>

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
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


/* A loopback UDP socket standing in for the receiver; records arrival time and FEC header. */
class wire_sniffer
{
public:
    struct shard
    {
        clock_type::time_point at;
        bool                   is_data = false;
        uint16_t               seq = 0; /* data: its sequence; parity: its block's first */
        int                    k_i = -1;
        int                    k_total = 0;
        int                    n = 0;
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
        /* Kernel receive stamps: a late wakeup of this thread must not bunch shards together. */
        const int on = 1;
        EXPECT_EQ(0, setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on)));
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
            iovec   iov {buf, sizeof(buf)};
            alignas(cmsghdr) uint8_t ctrl[CMSG_SPACE(sizeof(timespec))];
            msghdr  msg {};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctrl;
            msg.msg_controllen = sizeof(ctrl);
            const ssize_t n = recvmsg(fd_, &msg, MSG_DONTWAIT);
            rs_block_erasure::shard_header h;
            if (n <= 0 || !rs_block_erasure::parse_header(buf, static_cast<size_t>(n), &h))
            {
                ADD_FAILURE() << "bad shard";
                continue;
            }
            shard s;
            s.at = arrival_time(msg);
            s.is_data = h.is_data;
            s.seq = h.sequence_number;
            s.k_i = h.k_i;
            s.k_total = h.k_total;
            s.n = h.n;
            out.push_back(s);
        }
        return out;
    }

private:
    /* SO_TIMESTAMPNS stamps CLOCK_REALTIME; shift it onto clock_type by its age. */
    static clock_type::time_point arrival_time(msghdr &msg)
    {
        const auto now = clock_type::now();
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); nullptr != c; c = CMSG_NXTHDR(&msg, c))
        {
            if (SOL_SOCKET == c->cmsg_level && SCM_TIMESTAMPNS == c->cmsg_type)
            {
                timespec rx {};
                std::memcpy(&rx, CMSG_DATA(c), sizeof(rx));
                timespec wall {};
                clock_gettime(CLOCK_REALTIME, &wall);
                const int64_t age_ns = (static_cast<int64_t>(wall.tv_sec) - rx.tv_sec) *
                                           1'000'000'000LL +
                                       (wall.tv_nsec - rx.tv_nsec);
                if (age_ns >= 0)
                {
                    return now - std::chrono::nanoseconds(age_ns);
                }
                break;
            }
        }
        return now;
    }

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
    ASSERT_EQ(0, cfg(sender, "udp_transport", dst));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    /* k first: the default n (12) must stay >= k at every step. */
    ASSERT_EQ(0, cfg(sender, "fec_k", std::to_string(k)));
    ASSERT_EQ(0, cfg(sender, "fec_n", std::to_string(n)));
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", std::to_string(spread_ms)));
    ASSERT_EQ(0, sender.open());
}

/* One block's shards in send order: data at once, then parity. */
std::vector<std::vector<uint8_t>> encode_tags(rs_block_erasure &enc,
                                              const std::vector<uint8_t> &tags)
{
    std::vector<std::vector<uint8_t>> air;
    for (uint8_t t : tags)
    {
        const std::vector<uint8_t> app(32, t);
        rs_block_erasure::tx_out   out;
        enc.push_app(app.data(), app.size(), &out);
        for (const auto &b : out.shards)
        {
            air.emplace_back(b.u8(), b.u8() + b.size());
        }
    }
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
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    EXPECT_EQ(0, cfg(sender, "fec_spread_ms", "0"));
    EXPECT_EQ(0, cfg(sender, "fec_spread_ms", "40"));
    EXPECT_EQ(0, cfg(sender, "fec_spread_ms", "250"));
    /* Capped at the spread the receiver assumes before it has measured one. */
    EXPECT_EQ(-EINVAL, cfg(sender, "fec_spread_ms", "251"));
    EXPECT_EQ(-EINVAL, cfg(sender, "fec_spread_ms", "-1"));
    EXPECT_EQ(-EINVAL, cfg(sender, "fec_spread_ms", "x"));
    std::string v;
    ASSERT_EQ(0, sender.query("fec_spread_ms", &v));
    EXPECT_EQ("250", v);
}

/* spread 0: parity leaves right behind the data shard that closes the block. */
TEST(FecSpreadTest, ZeroSendsParityAtClose)
{
    wire_sniffer             sniff;
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    open_fec_sender(sender, sniff.host_port(), 4, 6, 0);
    for (uint8_t t = 0; t < 4; t++)
    {
        ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(t))));
    }
    const auto got = sniff.collect(6, 1000);
    ASSERT_EQ(6u, got.size());
    EXPECT_LT(ms_between(got.front().at, got.back().at), 5.0);
    for (size_t i = 0; i < 4; i++)
    {
        EXPECT_TRUE(got[i].is_data);
        EXPECT_EQ(static_cast<uint16_t>(got[0].seq + i), got[i].seq);
    }
    for (size_t i = 4; i < 6; i++)
    {
        EXPECT_FALSE(got[i].is_data);
        EXPECT_EQ(got[0].seq, got[i].seq);
        EXPECT_EQ(static_cast<int>(i - 4), got[i].k_i);
        EXPECT_EQ(4, got[i].k_total);
        EXPECT_EQ(2, got[i].n);
    }
    sender.close();
}

/* Spread 30 ms over 4 + 2 shards: data leaves at once; parity takes slots 4 and 5 of the window
 * (20 and 25 ms after the block's first data shard). */
TEST(FecSpreadTest, DataAtOnceParityOnItsSlots)
{
    wire_sniffer             sniff;
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    open_fec_sender(sender, sniff.host_port(), 4, 6, 30);
    const auto t0 = clock_type::now();
    for (uint8_t t = 0; t < 4; t++)
    {
        ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(t))));
    }
    const auto got = sniff.collect(6, 1000);
    ASSERT_EQ(6u, got.size());
    for (size_t i = 0; i < 4; i++)
    {
        EXPECT_TRUE(got[i].is_data);
        EXPECT_LT(ms_between(t0, got[i].at), 8.0) << "data shard " << i;
    }
    EXPECT_NEAR(ms_between(t0, got[4].at), 20.0, 6.0);
    EXPECT_NEAR(ms_between(t0, got[5].at), 25.0, 6.0);
    EXPECT_GT(ms_between(got[4].at, got[5].at), 2.0);
    sender.close();
}

/* A block that fills slower than its window: parity follows the close one slot apart instead of
 * leaving in a lump. */
TEST(FecSpreadTest, LateCloseKeepsParitySpacing)
{
    wire_sniffer             sniff;
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    open_fec_sender(sender, sniff.host_port(), 4, 6, 30);
    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(0))));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const auto close = clock_type::now();
    for (uint8_t t = 1; t < 4; t++)
    {
        ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(t))));
    }
    const auto got = sniff.collect(6, 1000);
    ASSERT_EQ(6u, got.size());
    EXPECT_LT(ms_between(close, got[4].at), 6.0);
    EXPECT_NEAR(ms_between(got[4].at, got[5].at), 5.0, 3.0);
    sender.close();
}

/* No FEC timeout: an open block's data has already left; its parity waits for the block to
 * fill. */
TEST(FecSpreadTest, OpenBlockSendsDataNoParity)
{
    wire_sniffer             sniff;
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    open_fec_sender(sender, sniff.host_port(), 8, 12, 0);
    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(1))));
    const auto got = sniff.collect(5, 150);
    ASSERT_EQ(1u, got.size());
    EXPECT_TRUE(got[0].is_data);
    sender.close();
}

/* Two blocks 10 ms apart with a 30 ms window: block B's data leaves before block A's parity. */
TEST(FecSpreadTest, OverlappingBlocksInterleave)
{
    wire_sniffer             sniff;
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    open_fec_sender(sender, sniff.host_port(), 4, 6, 30);
    for (uint8_t t = 0; t < 4; t++)
    {
        ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(t))));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    for (uint8_t t = 4; t < 8; t++)
    {
        ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(t))));
    }
    const auto got = sniff.collect(12, 1000);
    ASSERT_EQ(12u, got.size());
    const uint16_t first = got.front().seq;
    size_t         last_a_parity = 0;
    size_t         first_b = got.size();
    uint16_t       next_data = first;
    for (size_t i = 0; i < got.size(); i++)
    {
        if (!got[i].is_data && got[i].seq == first)
        {
            last_a_parity = i;
        }
        if (got[i].is_data)
        {
            EXPECT_EQ(next_data, got[i].seq);
            next_data++;
            if (got[i].seq == static_cast<uint16_t>(first + 4) && first_b == got.size())
            {
                first_b = i;
            }
        }
    }
    EXPECT_LT(first_b, last_a_parity) << "block B should start before block A's parity is out";
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

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:" + std::to_string(ntohs(addr.sin_port))));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", "40"));
    ASSERT_EQ(0, sender.open());

    const auto t0 = clock_type::now();
    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(7))));
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
    ASSERT_TRUE(enc.init(4, 6, 1476));
    enc.set_tx_sequence(96);
    const auto prior = encode_tags(enc, {90, 91, 92, 93});
    const auto a = encode_tags(enc, {1, 2, 3, 4});
    const auto b = encode_tags(enc, {5, 6, 7, 8});
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

/* Start-up edge case: a fresh receiver anchors in-order delivery on the first data shard it
 * sees. If the earlier block's first shard is lost and a later block's arrives first, the
 * earlier block is behind the reference: late, and ignored. */
TEST(FecSpreadTest, FreshReceiverAnchorsOnFirstShardSeen)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 1476));
    const auto a = encode_tags(enc, {1, 2, 3, 4});
    const auto b = encode_tags(enc, {5, 6, 7, 8});
    std::vector<std::vector<uint8_t>> wire;
    for (size_t i = 0; i < a.size(); i++)
    {
        wire.push_back(a[i]);
        wire.push_back(b[i]);
    }
    std::vector<bool> lost(wire.size(), false);
    lost[0] = true;  // A data 0: B data 0 is the first shard this receiver sees
    rs_block_erasure dec;
    EXPECT_EQ((std::vector<uint8_t> {5, 6, 7, 8}), decode_order(dec, wire, lost));
    EXPECT_GT(dec.late_blocks(), 0u);
}

/* A burst of 4 consecutive wire losses: sent back to back it takes 4 shards of one 4/6 block
 * (unrecoverable); interleaved over two blocks it takes 2 of each (both recover). */
TEST(FecSpreadTest, InterleavingSurvivesBurst)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 1476));
    const auto prior = encode_tags(enc, {90, 91, 92, 93});
    const auto a = encode_tags(enc, {1, 2, 3, 4});
    const auto b = encode_tags(enc, {5, 6, 7, 8});

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
    const std::vector<bool> none(prior.size(), false);

    rs_block_erasure dec_serial;
    (void)decode_order(dec_serial, prior, none);
    const auto got_serial = decode_order(dec_serial, serial, burst);
    EXPECT_LT(got_serial.size(), 8u);

    rs_block_erasure dec_mixed;
    (void)decode_order(dec_mixed, prior, none);
    EXPECT_EQ((std::vector<uint8_t> {1, 2, 3, 4, 5, 6, 7, 8}),
              decode_order(dec_mixed, mixed, burst));
}

/* End to end over loopback: sender with spreading, receiver decodes every packet in order. */
TEST(FecSpreadTest, LoopbackInOrderWithSpread)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender   sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
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
        ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(static_cast<uint8_t>(i)))));
        if (i % 7 == 6)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
    std::vector<uint8_t> tags;
    vstreamer::component_pdu out;
    const auto deadline = clock_type::now() + std::chrono::seconds(2);
    while (tags.size() < k_packets && clock_type::now() < deadline)
    {
        if (0 == receiver.output(out))
        {
            tags.push_back(static_cast<uint8_t>(
                vstreamer::test_pdu::stream_dgram_counter_be32(out.sdu.u8(), out.sdu.size())));
        }
    }
    ASSERT_EQ(static_cast<size_t>(k_packets), tags.size());
    for (int i = 0; i < k_packets; i++)
    {
        EXPECT_EQ(static_cast<uint32_t>(i), static_cast<uint32_t>(tags[static_cast<size_t>(i)]));
    }
    sender.close();
    receiver.close();
}
