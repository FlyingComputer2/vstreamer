#include "core/rs_block_erasure.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using vstreamer::rs_block_erasure;

namespace
{

constexpr size_t k_datagram = 1476;

using bytes = std::vector<uint8_t>;

struct shard
{
    bytes data;
    bool  parity = false;
};

/* App datagram i: its index in the first four bytes, then a pattern. */
bytes make_app(uint32_t i, size_t len)
{
    bytes b(std::max<size_t>(len, 4));
    std::memcpy(b.data(), &i, 4);
    for (size_t j = 4; j < b.size(); j++)
    {
        b[j] = static_cast<uint8_t>(i * 31 + j);
    }
    return b;
}

std::vector<shard> push(rs_block_erasure &enc, const bytes &app)
{
    rs_block_erasure::tx_out out;
    enc.push_app(app.data(), app.size(), &out);
    std::vector<shard> v;
    const size_t       data = out.shards.size() - static_cast<size_t>(out.parity);
    for (size_t i = 0; i < out.shards.size(); i++)
    {
        v.push_back({bytes(out.shards[i].u8(), out.shards[i].u8() + out.shards[i].size()),
                     i >= data});
    }
    return v;
}

std::vector<shard> flush(rs_block_erasure &enc)
{
    rs_block_erasure::tx_out out;
    enc.flush(&out);
    std::vector<shard> v;
    for (const auto &b : out.shards)
    {
        v.push_back({bytes(b.u8(), b.u8() + b.size()), true});
    }
    return v;
}

/* Encodes apps[0..count) and returns every shard in send order (data at once, parity after). */
std::vector<shard> encode_all(rs_block_erasure &enc, const std::vector<bytes> &apps)
{
    std::vector<shard> all;
    for (const auto &a : apps)
    {
        auto s = push(enc, a);
        all.insert(all.end(), s.begin(), s.end());
    }
    return all;
}

std::vector<bytes> make_apps(size_t count, std::mt19937 *rng, size_t max_len = 1400)
{
    std::uniform_int_distribution<size_t> len(1, max_len);
    std::vector<bytes>                    apps;
    for (size_t i = 0; i < count; i++)
    {
        apps.push_back(make_app(static_cast<uint32_t>(i), len(*rng)));
    }
    return apps;
}

struct receiver
{
    rs_block_erasure   dec;
    std::vector<bytes> got;
    void               take(vstreamer::fec_rx_payload_list &out)
    {
        for (const auto &p : out)
        {
            got.emplace_back(p.u8(), p.u8() + p.size());
        }
    }
    void push(const bytes &b)
    {
        vstreamer::fec_rx_payload_list out;
        dec.push_air(b.data(), b.size(), &out);
        take(out);
    }
    void poll()
    {
        vstreamer::fec_rx_payload_list out;
        dec.poll_rx(&out);
        take(out);
    }
};

/* Drives rs_block_erasure's clock: deadlines are tested by advancing it, not by sleeping. */
struct fake_clock
{
    std::chrono::steady_clock::time_point t =
        std::chrono::steady_clock::time_point {} + std::chrono::hours(1);
    void attach(rs_block_erasure &dec)
    {
        dec.set_clock([this] { return t; });
    }
    void advance(int ms)
    {
        t += std::chrono::milliseconds(ms);
    }
    void advance_us(int64_t us)
    {
        t += std::chrono::microseconds(us);
    }
};

rs_block_erasure make_enc(int k, int n, uint16_t seq = 100)
{
    rs_block_erasure enc;
    EXPECT_TRUE(enc.init(k, n, k_datagram));
    enc.set_tx_sequence(seq);
    return enc;
}

}  // namespace

TEST(RsBlockErasureWire, HeaderRoundTrip)
{
    uint8_t d[8] {};
    rs_block_erasure::write_data_header(d, 0xBEEF);
    EXPECT_EQ(d[0], 0x80);
    rs_block_erasure::shard_header h;
    ASSERT_TRUE(rs_block_erasure::parse_header(d, sizeof(d), &h));
    EXPECT_TRUE(h.is_data);
    EXPECT_EQ(h.sequence_number, 0xBEEF);
    EXPECT_EQ(h.header_len, 3u);

    uint8_t p[8] {};
    ASSERT_TRUE(rs_block_erasure::write_parity_header(p, 0x1234, 10, 21, 9));
    /* is_data 0 | n 01010 | k_total 10101 | k_i 01001 */
    EXPECT_EQ(p[0], 0x2A);
    EXPECT_EQ(p[1], 0xA9);
    ASSERT_TRUE(rs_block_erasure::parse_header(p, sizeof(p), &h));
    EXPECT_FALSE(h.is_data);
    EXPECT_EQ(h.sequence_number, 0x1234);
    EXPECT_EQ(h.n, 10);
    EXPECT_EQ(h.k_total, 21);
    EXPECT_EQ(h.k_i, 9);
    EXPECT_EQ(h.header_len, 4u);
}

TEST(RsBlockErasureWire, HeaderRejectsInvalid)
{
    rs_block_erasure::shard_header h;
    const uint8_t spare[] = {0x81, 0, 0, 1};
    EXPECT_FALSE(rs_block_erasure::parse_header(spare, sizeof(spare), &h));
    const uint8_t data_no_body[] = {0x80, 0, 0};
    EXPECT_FALSE(rs_block_erasure::parse_header(data_no_body, sizeof(data_no_body), &h));
    uint8_t p[8] {};
    EXPECT_FALSE(rs_block_erasure::write_parity_header(p, 0, 0, 4, 0));
    EXPECT_FALSE(rs_block_erasure::write_parity_header(p, 0, 2, 0, 0));
    EXPECT_FALSE(rs_block_erasure::write_parity_header(p, 0, 2, 4, 2));
    EXPECT_FALSE(rs_block_erasure::write_parity_header(p, 0, 32, 4, 0));
    /* k_i >= n on the wire. */
    const uint8_t bad_row[] = {0x04, 0x22, 0, 0, 1, 2, 3};
    EXPECT_FALSE(rs_block_erasure::parse_header(bad_row, sizeof(bad_row), &h));
}

TEST(RsBlockErasureTx, DataLeavesAtOnceParityAtClose)
{
    auto enc = make_enc(6, 8, 500);
    for (int i = 0; i < 5; i++)
    {
        auto s = push(enc, make_app(static_cast<uint32_t>(i), 100));
        ASSERT_EQ(s.size(), 1u);
        rs_block_erasure::shard_header h;
        ASSERT_TRUE(rs_block_erasure::parse_header(s[0].data.data(), s[0].data.size(), &h));
        EXPECT_TRUE(h.is_data);
        EXPECT_EQ(h.sequence_number, 500 + i);
        EXPECT_EQ(s[0].data.size(), 3u + 100u);
    }
    auto s = push(enc, make_app(5, 40));
    ASSERT_EQ(s.size(), 3u);
    EXPECT_FALSE(s[0].parity);
    for (int j = 0; j < 2; j++)
    {
        rs_block_erasure::shard_header h;
        ASSERT_TRUE(rs_block_erasure::parse_header(s[1 + j].data.data(), s[1 + j].data.size(),
                                                   &h));
        EXPECT_FALSE(h.is_data);
        EXPECT_EQ(h.sequence_number, 500);
        EXPECT_EQ(h.k_total, 6);
        EXPECT_EQ(h.n, 2);
        EXPECT_EQ(h.k_i, j);
        /* header + coded length + longest payload */
        EXPECT_EQ(s[1 + j].data.size(), 4u + 2u + 100u);
    }
    /* Next block starts right after. */
    auto next = push(enc, make_app(6, 10));
    rs_block_erasure::shard_header h;
    ASSERT_TRUE(rs_block_erasure::parse_header(next[0].data.data(), next[0].data.size(), &h));
    EXPECT_EQ(h.sequence_number, 506);
}

TEST(RsBlockErasureTx, SizeLimits)
{
    auto enc = make_enc(2, 3);
    EXPECT_EQ(enc.max_original(), k_datagram - 6);
    EXPECT_EQ(rs_block_erasure::max_data_payload(k_datagram), k_datagram - 3);
    EXPECT_TRUE(push(enc, make_app(0, k_datagram - 5)).empty());
    EXPECT_EQ(enc.oversized(), 1u);
    auto a = push(enc, make_app(1, k_datagram - 6));
    auto b = push(enc, make_app(2, 1));
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(a[0].data.size(), k_datagram - 3);
    EXPECT_EQ(b[1].data.size(), k_datagram);
}

TEST(RsBlockErasureTx, InitRejectsInvalid)
{
    rs_block_erasure enc;
    EXPECT_FALSE(enc.init(0, 2, k_datagram));
    EXPECT_FALSE(enc.init(32, 32, k_datagram));
    EXPECT_FALSE(enc.init(4, 3, k_datagram));
    EXPECT_FALSE(enc.init(4, 6, 10));
    EXPECT_TRUE(enc.init(1, 1, k_datagram));
    EXPECT_TRUE(enc.init(31, 31, k_datagram));
}

TEST(RsBlockErasureTx, BuffersHaveHeadroomAndAlignedBodies)
{
    auto enc = make_enc(3, 5);
    enc.set_tx_buffers(8, [](size_t cap) {
        auto b = bfc::shared_sized_buffer::allocate(cap + 64);
        b.resize(cap + 64);
        std::memset(b.u8(), 0xEE, b.size());
        return b;
    });
    rs_block_erasure::tx_out out;
    for (uint32_t i = 0; i < 3; i++)
    {
        const bytes app = make_app(i, 50 + i);
        enc.push_app(app.data(), app.size(), &out);
        ASSERT_FALSE(out.shards.empty());
        const auto &d = out.shards[0];
        EXPECT_EQ(d.size(), 8u + 3u + app.size());
        EXPECT_EQ(d.u8()[0], 0xEE); /* headroom untouched */
        EXPECT_EQ(reinterpret_cast<uintptr_t>(d.u8() + 8 + 3) % 16, 0u);
        EXPECT_EQ(0, std::memcmp(d.u8() + 11, app.data(), app.size()));
    }
    ASSERT_EQ(out.parity, 2);
    for (size_t j = 1; j < out.shards.size(); j++)
    {
        EXPECT_EQ(reinterpret_cast<uintptr_t>(out.shards[j].u8() + 8 + 4 + 2) % 16, 0u);
    }
}

TEST(RsBlockErasureRx, LosslessInOrder)
{
    std::mt19937 rng(1);
    auto         enc = make_enc(6, 8);
    auto         apps = make_apps(200, &rng);
    auto         all = encode_all(enc, apps);
    receiver     rx;
    for (const auto &s : all)
    {
        rx.push(s.data);
    }
    EXPECT_EQ(rx.got, apps);
    EXPECT_EQ(rx.dec.recovered(), 0u);
    EXPECT_EQ(rx.dec.decode_fail(), 0u);
    EXPECT_EQ(rx.dec.missing_shards(), 0u);
}

/* A receiver starts at the first data shard it sees, so the block under test follows a
 * complete one. */
TEST(RsBlockErasureRx, RecoversEveryPairOfErasures)
{
    std::mt19937 rng(2);
    for (int a = 0; a < 8; a++)
    {
        for (int b = a + 1; b < 8; b++)
        {
            auto     enc = make_enc(6, 8);
            auto     apps = make_apps(12, &rng);
            auto     all = encode_all(enc, apps);
            receiver rx;
            ASSERT_EQ(all.size(), 16u);
            for (int i = 0; i < 16; i++)
            {
                if (i != 8 + a && i != 8 + b)
                {
                    rx.push(all[static_cast<size_t>(i)].data);
                }
            }
            EXPECT_EQ(rx.got, apps) << "lost " << a << "," << b;
            EXPECT_EQ(rx.dec.decode_fail(), 0u);
        }
    }
}

TEST(RsBlockErasureRx, Kn20N31MaxErasures)
{
    std::mt19937 rng(3);
    auto         enc = make_enc(20, 31);
    auto         apps = make_apps(40, &rng);
    auto         all = encode_all(enc, apps);
    ASSERT_EQ(all.size(), 62u);
    std::vector<size_t> order(31);
    for (size_t i = 0; i < 31; i++)
    {
        order[i] = 31 + i;
    }
    std::shuffle(order.begin(), order.end(), rng);
    receiver rx;
    for (size_t i = 0; i < 31; i++)
    {
        rx.push(all[i].data);
    }
    for (size_t i = 0; i < 20; i++)
    {
        rx.push(all[order[i]].data);
    }
    EXPECT_EQ(rx.got, apps);
}

TEST(RsBlockErasureRx, ParityAfterNextBlockDataStillInOrder)
{
    std::mt19937 rng(4);
    auto         enc = make_enc(4, 6);
    auto         apps = make_apps(8, &rng);
    auto         all = encode_all(enc, apps); /* d0..d3 p0 p1 d4..d7 p0 p1 */
    ASSERT_EQ(all.size(), 12u);
    receiver rx;
    /* Block 0 loses d1; its parity is spread past block 1's data. */
    rx.push(all[0].data);
    rx.push(all[2].data);
    rx.push(all[3].data);
    rx.push(all[6].data);
    rx.push(all[7].data);
    EXPECT_EQ(rx.got.size(), 1u); /* d0; the rest waits behind the hole */
    rx.push(all[4].data);
    EXPECT_EQ(rx.got.size(), 6u);
    rx.push(all[8].data);
    rx.push(all[9].data);
    rx.push(all[5].data);
    rx.push(all[10].data);
    rx.push(all[11].data);
    EXPECT_EQ(rx.got, apps);
    EXPECT_EQ(rx.dec.recovered(), 1u);
}

TEST(RsBlockErasureRx, FlushedPartialBlockRecovers)
{
    std::mt19937 rng(5);
    auto         enc = make_enc(6, 9);
    auto         apps = make_apps(4, &rng);
    auto         all = encode_all(enc, apps);
    auto         parity = flush(enc);
    ASSERT_EQ(parity.size(), 3u);
    rs_block_erasure::shard_header h;
    ASSERT_TRUE(rs_block_erasure::parse_header(parity[0].data.data(), parity[0].data.size(), &h));
    EXPECT_EQ(h.k_total, 4);
    receiver rx;
    rx.push(all[0].data);
    rx.push(all[3].data);
    rx.push(parity[0].data);
    rx.push(parity[2].data);
    EXPECT_EQ(rx.got, apps);
    /* A new block continues the sequence. */
    auto next = push(enc, make_app(9, 10));
    ASSERT_TRUE(rs_block_erasure::parse_header(next[0].data.data(), next[0].data.size(), &h));
    EXPECT_EQ(h.sequence_number, static_cast<uint16_t>(100 + 4));
}

TEST(RsBlockErasureRx, RuntimeKnChangeKeepsSequence)
{
    std::mt19937 rng(6);
    auto         enc = make_enc(6, 8);
    auto         apps = make_apps(40, &rng);
    std::vector<shard> all;
    for (size_t i = 0; i < apps.size(); i++)
    {
        if (i == 15)
        {
            auto f = flush(enc);
            all.insert(all.end(), f.begin(), f.end());
            ASSERT_TRUE(enc.init(4, 7, k_datagram));
        }
        auto s = push(enc, apps[i]);
        all.insert(all.end(), s.begin(), s.end());
    }
    auto tail = flush(enc);
    all.insert(all.end(), tail.begin(), tail.end());
    receiver rx;
    for (size_t i = 0; i < all.size(); i++)
    {
        if (i % 5 != 2) /* lose a fifth of the shards, spread out */
        {
            rx.push(all[i].data);
        }
    }
    EXPECT_EQ(rx.got, apps);
    EXPECT_EQ(rx.dec.decode_fail(), 0u);
}

TEST(RsBlockErasureRx, SequenceWrapNoLoss)
{
    std::mt19937 rng(7);
    auto         enc = make_enc(5, 7, 65530);
    auto         apps = make_apps(30, &rng, 200);
    auto         all = encode_all(enc, apps);
    receiver     rx;
    for (size_t i = 0; i < all.size(); i++)
    {
        if (i != 3 && i != 10)
        {
            rx.push(all[i].data);
        }
    }
    EXPECT_EQ(rx.got, apps);
}

TEST(RsBlockErasureRx, HoleWithoutParityGivenUpAtCapThenAtEstimate)
{
    fake_clock clk;
    receiver   rx;
    clk.attach(rx.dec);
    std::mt19937 rng(8);
    auto         enc = make_enc(4, 6);
    auto         apps = make_apps(36, &rng, 100);
    auto         all = encode_all(enc, apps); /* 9 blocks of 6 shards */
    /* Block 0 loses d2 and both parity: nothing can recover it. */
    for (size_t i = 0; i < 6; i++)
    {
        if (i == 2 || i >= 4)
        {
            continue;
        }
        rx.push(all[i].data);
        clk.advance(1);
    }
    EXPECT_EQ(rx.got.size(), 2u);
    /* No estimate yet: the hole waits about the 250 ms cap from its block's close. */
    clk.advance(200);
    rx.poll();
    EXPECT_EQ(rx.got.size(), 2u);
    clk.advance(60);
    rx.poll();
    EXPECT_EQ(rx.got.size(), 3u);
    EXPECT_EQ(rx.dec.given_up_blocks(), 1u);
    EXPECT_EQ(rx.dec.take_fail_lost_app_pkts(), 1u);

    /* k = 4, n = 2 (c = 6), spread 12 ms: row gap 2 ms. The sender releases parity row j at
     * max(close + 2j, first + 8 + 2j) ms. Slowly filled blocks (data 3 ms apart) get their parity
     * right after the close, at +0 and +2 ms. */
    for (size_t b = 1; b < 6; b++)
    {
        for (size_t i = 0; i < 4; i++)
        {
            if (i > 0)
            {
                clk.advance(3);
            }
            rx.push(all[b * 6 + i].data);
        }
        rx.push(all[b * 6 + 4].data);
        clk.advance(2);
        rx.push(all[b * 6 + 5].data);
        clk.advance(10);
    }
    ASSERT_TRUE(rx.dec.have_spread_estimate());
    EXPECT_NEAR(rx.dec.spread_estimate_ms(), 12.0, 0.5);

    /* Block 6 fills in a burst (fill 0.2 ms): its parity is due at close + 7.8 and + 9.8 ms. It
     * loses d1 and parity row 0; row 1 still recovers it, though the slow blocks taught nothing
     * longer than 2 ms. */
    size_t before = rx.got.size();
    rx.push(all[36].data);
    clk.advance_us(100);
    rx.push(all[38].data);
    clk.advance_us(100);
    rx.push(all[39].data); /* the close */
    clk.advance(9);
    rx.poll();
    EXPECT_EQ(rx.got.size(), before + 1);
    clk.advance_us(800);
    rx.push(all[41].data);
    EXPECT_EQ(rx.got.size(), before + 4);
    EXPECT_EQ(rx.dec.given_up_blocks(), 1u);
    clk.advance(10);

    /* Block 7, also a burst, loses d1 and both parity: given up after close + 9.8 ms + margin,
     * not at the 250 ms cap. */
    before = rx.got.size();
    rx.push(all[42].data);
    clk.advance_us(100);
    rx.push(all[44].data);
    clk.advance_us(100);
    rx.push(all[45].data);
    clk.advance(12);
    rx.poll();
    EXPECT_EQ(rx.got.size(), before + 1);
    clk.advance(10);
    rx.poll();
    EXPECT_EQ(rx.got.size(), before + 3);
    clk.advance(10);

    /* Block 8 fills slowly and loses d1 and both parity: its parity was due at close + 2 ms, so
     * it is given up well before a burst block would be. */
    before = rx.got.size();
    rx.push(all[48].data);
    clk.advance(6);
    rx.push(all[50].data);
    clk.advance(3);
    rx.push(all[51].data);
    clk.advance(4);
    rx.poll();
    EXPECT_EQ(rx.got.size(), before + 1);
    clk.advance(6);
    rx.poll();
    EXPECT_EQ(rx.got.size(), before + 3);
    EXPECT_EQ(rx.dec.given_up_blocks(), 3u);
}

TEST(RsBlockErasureRx, DataOnlyStreamSkipsHoleOnNextData)
{
    auto     enc = make_enc(1, 1);
    receiver rx;
    std::vector<bytes> sent;
    for (uint32_t i = 0; i < 300; i++)
    {
        const bytes a = make_app(i, 20);
        auto        s = push(enc, a);
        ASSERT_EQ(s.size(), 1u);
        if (i == 280)
        {
            continue;
        }
        sent.push_back(a);
        rx.push(s[0].data);
    }
    /* Past the data-only threshold a hole goes as soon as later data shows up. */
    EXPECT_EQ(rx.got, sent);
    EXPECT_EQ(rx.dec.missing_shards(), 1u);
}

TEST(RsBlockErasureRx, MissingParityCountedWhenBlockDone)
{
    fake_clock clk;
    receiver   rx;
    clk.attach(rx.dec);
    std::mt19937 rng(9);
    auto         enc = make_enc(4, 6);
    auto         all = encode_all(enc, make_apps(8, &rng, 50));
    for (size_t i = 0; i < all.size(); i++)
    {
        if (i != 5)
        {
            rx.push(all[i].data);
        }
    }
    EXPECT_EQ(rx.dec.take_missing_parity(), 0u);
    clk.advance(300);
    rx.poll();
    EXPECT_EQ(rx.dec.take_missing_parity(), 1u);
    EXPECT_EQ(rx.dec.decode_fail(), 0u);
}

TEST(RsBlockErasureRx, PeerRestartTakesNewReference)
{
    std::mt19937 rng(10);
    auto         a = make_enc(4, 6, 1000);
    auto         b = make_enc(4, 6, 30000);
    auto         apps_a = make_apps(10, &rng);
    auto         apps_b = make_apps(10, &rng);
    receiver     rx;
    for (const auto &s : encode_all(a, apps_a))
    {
        rx.push(s.data);
    }
    for (const auto &s : encode_all(b, apps_b))
    {
        rx.push(s.data);
    }
    std::vector<bytes> want = apps_a;
    /* a's last two data shards wait in their open block: they were released in order. */
    want.insert(want.end(), apps_b.begin(), apps_b.end());
    EXPECT_EQ(rx.got, want);
    EXPECT_EQ(rx.dec.new_references(), 1u);
}

TEST(RsBlockErasureRx, LateDataBehindReleaseIgnored)
{
    fake_clock clk;
    receiver   rx;
    clk.attach(rx.dec);
    auto     enc = make_enc(1, 1);
    std::vector<bytes> wire;
    for (uint32_t i = 0; i < 300; i++)
    {
        wire.push_back(push(enc, make_app(i, 10))[0].data);
    }
    for (size_t i = 0; i < 300; i++)
    {
        if (i != 290)
        {
            rx.push(wire[i]);
        }
    }
    ASSERT_EQ(rx.got.size(), 299u);
    rx.push(wire[290]);
    EXPECT_EQ(rx.got.size(), 299u);
    EXPECT_EQ(rx.dec.late_blocks(), 1u);
    EXPECT_EQ(rx.dec.new_references(), 0u);
}

TEST(RsBlockErasureRx, KnMismatchCounted)
{
    receiver rx;
    uint8_t  p[4 + 2 + 16] {};
    ASSERT_TRUE(rs_block_erasure::write_parity_header(p, 50, 2, 4, 0));
    rx.push(bytes(p, p + sizeof(p)));
    ASSERT_TRUE(rs_block_erasure::write_parity_header(p, 50, 3, 4, 1));
    rx.push(bytes(p, p + sizeof(p)));
    EXPECT_EQ(rx.dec.kn_mismatch(), 1u);
    /* The replaced block is counted: it got one of its two parity shards. */
    EXPECT_EQ(rx.dec.take_missing_parity(), 1u);
    const uint8_t junk[] = {0x7F};
    rx.push(bytes(junk, junk + 1));
    EXPECT_EQ(rx.dec.hdr_errors(), 1u);
}

TEST(RsBlockErasureRx, LateParityFarBehindDropped)
{
    std::mt19937 rng(12);
    auto         enc = make_enc(4, 5);
    auto         apps = make_apps(204, &rng, 50);
    auto         all = encode_all(enc, apps); /* 51 blocks of 5 shards */
    receiver     rx;
    for (size_t i = 0; i < 250; i++)
    {
        if (i != 4)
        {
            rx.push(all[i].data);
        }
    }
    ASSERT_EQ(rx.got.size(), 200u);
    /* Block 0's parity arrives 196 data shards late: dropped, not a new session. */
    rx.push(all[4].data);
    EXPECT_EQ(rx.dec.new_references(), 0u);
    for (size_t i = 250; i < 255; i++)
    {
        rx.push(all[i].data);
    }
    EXPECT_EQ(rx.got, apps);
}

TEST(RsBlockErasureRx, DuplicateParityOfDoneBlockIgnored)
{
    fake_clock clk;
    receiver   rx;
    clk.attach(rx.dec);
    std::mt19937 rng(13);
    auto         enc = make_enc(4, 6);
    auto         all = encode_all(enc, make_apps(8, &rng, 50));
    for (const auto &s : all)
    {
        rx.push(s.data);
    }
    clk.advance(300);
    rx.poll();
    EXPECT_EQ(rx.dec.take_missing_parity(), 0u);
    rx.push(all[5].data);
    clk.advance(300);
    rx.poll();
    EXPECT_EQ(rx.dec.take_missing_parity(), 0u);
    EXPECT_EQ(rx.dec.decode_fail(), 0u);
}

TEST(RsBlockErasureRx, DataOnlyRunOfHolesIsOneBlock)
{
    auto     enc = make_enc(1, 1);
    receiver rx;
    for (uint32_t i = 0; i < 300; i++)
    {
        auto s = push(enc, make_app(i, 20));
        if (i < 280 || i >= 290)
        {
            rx.push(s[0].data);
        }
    }
    EXPECT_EQ(rx.got.size(), 290u);
    EXPECT_EQ(rx.dec.missing_shards(), 10u);
    EXPECT_EQ(rx.dec.given_up_blocks(), 1u);
}

TEST(RsBlockErasureRx, ShortFlushedBlockKeepsNextBlockWait)
{
    fake_clock clk;
    receiver   rx;
    clk.attach(rx.dec);
    std::mt19937 rng(14);
    auto         enc = make_enc(8, 10);
    auto         apps = make_apps(27, &rng, 100);
    /* Blocks 100 and 108 (k = 8) teach a spread: parity 1 and 2 ms after the close. */
    for (size_t b = 0; b < 2; b++)
    {
        std::vector<shard> parity;
        for (size_t i = 0; i < 8; i++)
        {
            auto s = push(enc, apps[b * 8 + i]);
            rx.push(s[0].data);
            parity.assign(s.begin() + 1, s.end());
            clk.advance(1);
        }
        for (const auto &p : parity)
        {
            rx.push(p.data);
            clk.advance(1);
        }
        clk.advance(5);
    }
    /* A reconfigure flushes block 116 after 3 data shards. */
    for (size_t i = 16; i < 19; i++)
    {
        rx.push(push(enc, apps[i])[0].data);
        clk.advance(1);
    }
    for (const auto &p : flush(enc))
    {
        rx.push(p.data);
        clk.advance(1);
    }
    ASSERT_TRUE(rx.dec.have_spread_estimate());
    /* Block 119 (k = 8 again) loses 120 and fills slowly. Taken as a 3-long block, its close
     * would be 121's arrival and 120 would be given up long before the parity comes. */
    std::vector<shard> parity;
    for (size_t i = 19; i < 27; i++)
    {
        auto s = push(enc, apps[i]);
        if (i != 20)
        {
            rx.push(s[0].data);
        }
        parity.assign(s.begin() + 1, s.end());
        clk.advance(10);
    }
    ASSERT_EQ(parity.size(), 2u);
    rx.push(parity[0].data);
    EXPECT_EQ(rx.got, apps);
    EXPECT_EQ(rx.dec.given_up_blocks(), 0u);
}

TEST(RsBlockErasureRx, RandomLossStreamRecoversWithinParity)
{
    std::mt19937 rng(11);
    auto         enc = make_enc(10, 14);
    auto         apps = make_apps(1000, &rng);
    std::vector<shard> all;
    std::vector<int>   block_of_shard;
    int                block = 0;
    for (const auto &a : apps)
    {
        auto s = push(enc, a);
        for (const auto &x : s)
        {
            all.push_back(x);
            block_of_shard.push_back(block);
        }
        if (s.size() > 1)
        {
            block++;
        }
    }
    /* Lose up to 4 shards of each block (any of its 14). */
    std::vector<int> lost(static_cast<size_t>(block + 1), 0);
    std::bernoulli_distribution drop(0.2);
    receiver rx;
    for (size_t i = 0; i < all.size(); i++)
    {
        const int b = block_of_shard[i];
        if (drop(rng) && lost[static_cast<size_t>(b)] < 4)
        {
            lost[static_cast<size_t>(b)]++;
            continue;
        }
        rx.push(all[i].data);
    }
    EXPECT_EQ(rx.got, apps);
    EXPECT_EQ(rx.dec.decode_fail(), 0u);
    EXPECT_GT(rx.dec.recovered(), 0u);
}
