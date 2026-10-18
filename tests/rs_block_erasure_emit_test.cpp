#include "core/rs_block_erasure.hpp"

#include <gtest/gtest.h>


#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using vstreamer::rs_block_erasure;

namespace
{

std::vector<uint8_t> encode_one(rs_block_erasure &enc, uint16_t sdu_base, uint8_t tag)
{
    const std::vector<uint8_t> app = {tag};
    std::vector<std::vector<uint8_t>> air;
    EXPECT_TRUE(enc.encode_block({app}, sdu_base, &air));
    EXPECT_EQ(air.size(), 1u);
    return air[0];
}

std::vector<std::vector<uint8_t>> encode_block_apps(rs_block_erasure &enc, uint16_t sdu_base,
                                                    const std::vector<uint8_t> &tags)
{
    std::vector<std::vector<uint8_t>> apps;
    for (uint8_t t : tags)
    {
        apps.push_back({t});
    }
    std::vector<std::vector<uint8_t>> air;
    EXPECT_TRUE(enc.encode_block(apps, sdu_base, &air));
    return air;
}

int fec_shard_index(const std::vector<uint8_t> &shard)
{
    uint16_t base = 0;
    int      idx = 0;
    int      k = 0;
    int      n = 0;
    int      sn = 0;
    EXPECT_TRUE(rs_block_erasure::unpack_header(shard.data(), shard.size(), &base, &idx, &k, &n,
                                              &sn));
    return idx;
}

bool out_has_app_tag(const vstreamer::fec_rx_payload_list &rows, uint8_t tag)
{
    for (const auto &row : rows)
    {
        if (!row.empty() && row.u8()[0] == tag)
        {
            return true;
        }
    }
    return false;
}

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

/* Past any deadline set before a spread estimate exists (spread cap + margin). */
constexpr int k_past_deadline_ms =
    rs_block_erasure::k_spread_cap_ms + rs_block_erasure::k_deadline_margin_min_ms + 10;

void feed_append(rs_block_erasure &dec, const std::vector<uint8_t> &shard,
                 vstreamer::fec_rx_payload_list *accum)
{
    vstreamer::fec_rx_payload_list step;
    dec.push_air(shard.data(), shard.size(), &step);
    accum->insert(accum->end(), std::make_move_iterator(step.begin()),
                  std::make_move_iterator(step.end()));
}

}  // namespace

TEST(RsBlockErasureEmitTest, SplitCountersHdrAndKnMismatch)
{
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    const uint8_t                     short_buf[2] = {0, 0};
    dec.push_air(short_buf, sizeof(short_buf), &out);
    EXPECT_EQ(dec.hdr_errors(), 1u);
    EXPECT_EQ(dec.decode_fail(), 0u);

    rs_block_erasure enc_a;
    rs_block_erasure enc_b;
    ASSERT_TRUE(enc_a.init(4, 6, 20));
    ASSERT_TRUE(enc_b.init(3, 5, 20));
    const auto air_a = encode_block_apps(enc_a, 3, {1, 2, 3, 4});
    const auto air_b = encode_block_apps(enc_b, 3, {9});
    feed_append(dec, air_a[0], &out);
    feed_append(dec, air_b[0], &out);
    EXPECT_EQ(dec.kn_mismatch(), 1u);
    EXPECT_EQ(dec.decode_fail(), 0u);
}

TEST(RsBlockErasureEmitTest, MissingShardsOnRingEvict)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure enc_fill;
    ASSERT_TRUE(enc_fill.init(1, 1, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    /* Hold far longer than the test: the partial block must leave through ring eviction. With
     * the default 60 ms head-of-line hold, a slow run (Debug, loaded host) gives it up first and
     * counts it as given_up instead. */
    ASSERT_TRUE(dec.init(4, 6, 60000));
    const auto partial = encode_block_apps(enc, 5, {55, 56, 57, 58})[0];
    vstreamer::fec_rx_payload_list out;
    feed_append(dec, partial, &out);
    for (int i = 0; i < 2200; i++)
    {
        feed_append(dec, encode_one(enc_fill, static_cast<uint16_t>(100 + i),
                                    static_cast<uint8_t>(i & 0xFF)),
                    &out);
    }
    EXPECT_TRUE(dec.evicted_blocks() > 0u || dec.missing_shards() > 0u);
    EXPECT_GT(dec.missing_shards(), 0u);
    EXPECT_EQ(dec.decode_fail(), dec.evicted_blocks() + dec.rs_failures());
}

TEST(RsBlockErasureEmitTest, InitRejectsInvalidPreservesConfig)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(6, 8, 20));
    EXPECT_TRUE(enc.enabled());
    EXPECT_EQ(enc.k(), 6);
    EXPECT_EQ(enc.n(), 8);
    EXPECT_FALSE(enc.init(32, 32, 20));
    EXPECT_TRUE(enc.enabled());
    EXPECT_EQ(enc.k(), 6);
    EXPECT_EQ(enc.n(), 8);
}

/* Across the u16 wrap: a head block that arrives after its successors releases them in order. */
TEST(RsBlockErasureEmitTest, WrapLateHeadReleasesQueue)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(1, 1, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);

    vstreamer::fec_rx_payload_list all;
    for (uint16_t id = 65530; id <= 65533; ++id)
    {
        feed_append(dec, encode_one(enc, id, static_cast<uint8_t>(id)), &all);
    }
    for (const uint16_t id : {uint16_t {65535}, uint16_t {0}, uint16_t {1}})
    {
        feed_append(dec, encode_one(enc, id, static_cast<uint8_t>(id)), &all);
    }
    EXPECT_EQ(4u, all.size());
    vstreamer::fec_rx_payload_list late_step;
    const std::vector<uint8_t>     late_shard = encode_one(enc, 65534, 0xfe);
    dec.push_air(late_shard.data(), late_shard.size(), &late_step);
    ASSERT_EQ(4u, late_step.size());
    EXPECT_EQ(0xfe, late_step[0].u8()[0]);
    EXPECT_EQ(0xff, late_step[1].u8()[0]);
    EXPECT_EQ(0x00, late_step[2].u8()[0]);
    EXPECT_EQ(0x01, late_step[3].u8()[0]);
}

TEST(RsBlockErasureEmitTest, WrapInOrderNoLoss)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(1, 1, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    std::mt19937                    rng(42);
    std::vector<std::vector<uint8_t>> shards;
    for (int i = 0; i < 1000; i++)
    {
        shards.push_back(encode_one(enc, static_cast<uint16_t>(i), static_cast<uint8_t>(i & 0xFF)));
    }
    std::vector<int> order(1000);
    for (int i = 0; i < 1000; i++)
    {
        order[i] = i;
    }
    for (int i = 0; i < 1000; i++)
    {
        const int j = i + static_cast<int>(rng() % 3);
        if (j < 1000)
        {
            std::swap(order[i], order[j]);
        }
    }
    vstreamer::fec_rx_payload_list out;
    for (int idx : order)
    {
        feed_append(dec, shards[static_cast<size_t>(idx)], &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    ASSERT_EQ(out.size(), 1000u);
    for (size_t i = 0; i < out.size(); i++)
    {
        EXPECT_EQ(out[i].u8()[0], static_cast<uint8_t>(i & 0xFF));
    }
}

TEST(RsBlockErasureEmitTest, HeadOfLineStreaming)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto       air = encode_block_apps(enc, 10, {10, 11, 12, 13});
    vstreamer::fec_rx_payload_list out;
    for (size_t i = 0; i < air.size(); i++)
    {
        if (i == 2)
        {
            continue;
        }
        const int idx = fec_shard_index(air[i]);
        if (idx >= 4)
        {
            continue;
        }
        feed_append(dec, air[i], &out);
    }
    EXPECT_TRUE(out_has_app_tag(out, 10));
    EXPECT_TRUE(out_has_app_tag(out, 11));
    EXPECT_FALSE(out_has_app_tag(out, 12));
    EXPECT_FALSE(out_has_app_tag(out, 13));

    /* A later block is held behind the incomplete head until the head's deadline. */
    const auto later = encode_block_apps(enc, 11, {14, 15, 16, 17});
    feed_append(dec, later[0], &out);
    EXPECT_FALSE(out_has_app_tag(out, 14));

    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list tick;
    dec.poll_rx(&tick);
    out.insert(out.end(), tick.begin(), tick.end());
    EXPECT_TRUE(out_has_app_tag(out, 13));
    EXPECT_FALSE(out_has_app_tag(out, 12));
}

TEST(RsBlockErasureEmitTest, RecoveredStillInOrder)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto       air = encode_block_apps(enc, 20, {20, 21, 22, 23});
    vstreamer::fec_rx_payload_list out;
    for (size_t i = 0; i < air.size(); i++)
    {
        const int idx = fec_shard_index(air[i]);
        if (idx == 1)
        {
            continue;
        }
        feed_append(dec, air[i], &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    ASSERT_EQ(out.size(), 4u);
    for (int t = 0; t < 4; t++)
    {
        EXPECT_TRUE(out_has_app_tag(out, static_cast<uint8_t>(20 + t)));
    }
}

/* A restarted sender whose random start lands more than k_max_lo_block_id_deviation behind the
 * reference starts a new reference at once. */
TEST(RsBlockErasureEmitTest, PeerRestartResync)
{
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    rs_block_erasure enc_a;
    rs_block_erasure enc_b;
    ASSERT_TRUE(enc_a.init(1, 1, 20));
    ASSERT_TRUE(enc_b.init(1, 1, 20));

    vstreamer::fec_rx_payload_list out;
    for (int i = 0; i < 300; i++)
    {
        feed_append(dec, encode_one(enc_a, static_cast<uint16_t>(i), static_cast<uint8_t>(i & 0xFF)),
                    &out);
    }
    clk.advance(300);
    for (int i = 0; i < 10; i++)
    {
        feed_append(dec, encode_one(enc_b, static_cast<uint16_t>(100 + i),
                                    static_cast<uint8_t>((100 + i) & 0xFF)),
                    &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    EXPECT_GE(out.size(), 310u);
    EXPECT_EQ(1u, dec.new_references());
}

/* The accepted corner: a restart landing behind but within k_max_lo_block_id_deviation reads as
 * late shards and is ignored (odds per restart lo / 65536). */
TEST(RsBlockErasureEmitTest, RestartBehindWithinLoIgnoredAsLate)
{
    rs_block_erasure dec;
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(1, 1, 20));
    vstreamer::fec_rx_payload_list out;
    for (int i = 0; i < 300; i++)
    {
        feed_append(dec, encode_one(enc, static_cast<uint16_t>(i), 1), &out);
    }
    for (int i = 0; i < 10; i++)
    {
        feed_append(dec, encode_one(enc, static_cast<uint16_t>(250 + i), 2), &out);
    }
    EXPECT_EQ(300u, out.size());
    EXPECT_EQ(10u, dec.late_blocks());
    EXPECT_EQ(0u, dec.new_references());
}

/* A restarted sender whose start lands ahead of the reference but within
 * k_max_hi_block_id_deviation reads as the same session with a gap: the gap is skipped at its
 * deadline, then the new blocks flow in order. */
TEST(RsBlockErasureEmitTest, PeerRestartResyncForwardJump)
{
    for (const int offset : {10, 80, 120})
    {
        rs_block_erasure dec;
        fake_clock       clk;
        clk.attach(dec);
        rs_block_erasure enc_a;
        rs_block_erasure enc_b;
        ASSERT_TRUE(enc_a.init(6, 8, 20));
        ASSERT_TRUE(enc_b.init(6, 8, 20));

        vstreamer::fec_rx_payload_list out;
        uint16_t                       tx_base = 0;
        for (int i = 0; i < 67; i++)
        {
            for (const auto &shard : encode_block_apps(enc_a, tx_base, {1, 1, 1, 1, 1, 1}))
            {
                feed_append(dec, shard, &out);
            }
            tx_base = static_cast<uint16_t>(tx_base + 6);
        }
        ASSERT_EQ(out.size(), 67u * 6u);
        clk.advance(300);

        const uint16_t start = static_cast<uint16_t>(66 * 6 + offset);
        for (int i = 0; i < 20; i++)
        {
            const uint16_t id = static_cast<uint16_t>(start + static_cast<uint16_t>(i * 6));
            const uint8_t  tag = static_cast<uint8_t>(100 + i);
            for (const auto &shard :
                 encode_block_apps(enc_b, id, {tag, tag, tag, tag, tag, tag}))
            {
                feed_append(dec, shard, &out);
            }
        }
        clk.advance(k_past_deadline_ms);
        vstreamer::fec_rx_payload_list tick;
        dec.poll_rx(&tick);
        out.insert(out.end(), tick.begin(), tick.end());
        ASSERT_EQ(out.size(), 67u * 6u + 20u * 6u) << "offset " << offset;
        for (int i = 0; i < 20; i++)
        {
            EXPECT_EQ(out[67u * 6u + static_cast<size_t>(i) * 6u].u8()[0], 100 + i);
        }
        EXPECT_EQ(0u, dec.new_references());
    }
}

TEST(RsBlockErasureEmitTest, RingEvictsStalePartial)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure enc_fill;
    ASSERT_TRUE(enc_fill.init(1, 1, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto air = encode_block_apps(enc, 5, {55, 56, 57, 58});
    const std::vector<uint8_t> partial = air[0];
    vstreamer::fec_rx_payload_list out;
    feed_append(dec, partial, &out);
    for (int i = 0; i < 2200; i++)
    {
        feed_append(dec, encode_one(enc_fill, static_cast<uint16_t>(100 + i),
                                    static_cast<uint8_t>(i & 0xFF)),
                    &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    for (const auto &shard : encode_block_apps(enc, 5, {77}))
    {
        feed_append(dec, shard, &out);
    }
    EXPECT_TRUE(out_has_app_tag(out, 77));
}

TEST(RsBlockErasureEmitTest, NextDeadlineAfterPartialPush)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 100));
    std::vector<std::vector<uint8_t>> air;
    enc.push_app(reinterpret_cast<const uint8_t *>("x"), 1, &air);
    EXPECT_TRUE(air.empty());

    std::chrono::steady_clock::time_point dl;
    EXPECT_TRUE(enc.next_deadline(&dl));
    EXPECT_GT(dl, std::chrono::steady_clock::now());

    enc.flush(&air);
    EXPECT_FALSE(enc.next_deadline(&dl));
}



/* Late parity for a block that already decoded while an earlier block was incomplete must not
 * create a phantom partial block whose abandonment counts the whole block as lost again. */
TEST(RsBlockErasureEmitTest, LateParityOfReadyBlockNotCountedLost)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto b0 = encode_block_apps(enc, 0, {1, 2, 3, 4});
    const auto b1 = encode_block_apps(enc, 4, {5, 6, 7, 8});
    vstreamer::fec_rx_payload_list out;
    feed_append(dec, b0[0], &out); /* block 0: 1 of 6 shards, 3 apps truly lost */
    for (size_t i = 0; i < 4; i++)
    {
        feed_append(dec, b1[i], &out); /* block 1 decodes while block 0 is incomplete */
    }
    for (size_t i = 4; i < 6; i++)
    {
        feed_append(dec, b1[i], &out); /* its parity arrives afterwards */
    }
    for (int round = 0; round < 2; round++)
    {
        clk.advance(400);
        vstreamer::fec_rx_payload_list flush;
        dec.poll_rx(&flush);
        out.insert(out.end(), flush.begin(), flush.end());
    }
    EXPECT_EQ(5U, out.size());
    EXPECT_EQ(3U, dec.take_fail_lost_app_pkts());
    EXPECT_EQ(1U, dec.given_up_blocks()); /* block 0 flushed at its deadline */
    EXPECT_EQ(0U, dec.take_evicted_blocks()); /* no phantom block 1 */
}

TEST(RsBlockErasureEmitTest, Kn31RoundTripMaxErasures)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(31, 31, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    std::vector<uint8_t> tags;
    for (int i = 0; i < 31; i++)
    {
        tags.push_back(static_cast<uint8_t>(i));
    }
    const auto air = encode_block_apps(enc, 1000, tags);
    ASSERT_EQ(air.size(), 31u);
    vstreamer::fec_rx_payload_list out;
    for (const auto &shard : air)
    {
        feed_append(dec, shard, &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    ASSERT_EQ(out.size(), 31u);
}

TEST(RsBlockErasureEmitTest, Kn20N31MaxErasuresRoundTrip)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(20, 31, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    std::vector<uint8_t> tags(20);
    for (int i = 0; i < 20; i++)
    {
        tags[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    }
    const auto air = encode_block_apps(enc, 200, tags);
    vstreamer::fec_rx_payload_list out;
    for (size_t i = 0; i < air.size(); i++)
    {
        if (i < 11)
        {
            continue;
        }
        feed_append(dec, air[i], &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    ASSERT_EQ(out.size(), 20u);
}

TEST(RsBlockErasureEmitTest, Kn20N31PartialThenFullNoFalseGap)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(20, 31, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    std::vector<uint8_t> partial_tags(3, 7);
    const auto           partial_air = encode_block_apps(enc, 50, partial_tags);
    std::vector<uint8_t> full_tags(20, 8);
    const auto           full_air = encode_block_apps(enc, 53, full_tags);
    vstreamer::fec_rx_payload_list out;
    for (const auto &s : partial_air)
    {
        feed_append(dec, s, &out);
    }
    for (const auto &s : full_air)
    {
        feed_append(dec, s, &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u);
    EXPECT_EQ(out.size(), 23u);
}

TEST(RsBlockErasureEmitTest, WholeBlockLostCountsSduN)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto a = encode_block_apps(enc, 0, {1, 2, 3, 4});
    const auto c = encode_block_apps(enc, 8, {9, 10, 11, 12});
    vstreamer::fec_rx_payload_list out;
    for (const auto &s : a)
    {
        feed_append(dec, s, &out);
    }
    for (const auto &s : c)
    {
        feed_append(dec, s, &out);
    }
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list tick;
    dec.poll_rx(&tick);
    out.insert(out.end(), tick.begin(), tick.end());
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 4u);
    EXPECT_EQ(out.size(), 8u);
}

TEST(RsBlockErasureEmitTest, ConsecutiveBlocksLostCountsSumSduN)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(2, 4, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto a = encode_block_apps(enc, 0, {1, 2});
    const auto d = encode_block_apps(enc, 8, {9, 10});
    vstreamer::fec_rx_payload_list out;
    for (const auto &s : a)
    {
        feed_append(dec, s, &out);
    }
    for (const auto &s : d)
    {
        feed_append(dec, s, &out);
    }
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list tick;
    dec.poll_rx(&tick);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 6u);
}

TEST(RsBlockErasureEmitTest, RuntimeKnChangeSduBaseContinuity)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(2, 4, 20));
    uint16_t base = 0;
    const auto b0 = encode_block_apps(enc, base, {1, 2});
    base = static_cast<uint16_t>(base + 2);
    ASSERT_TRUE(enc.init(3, 5, 20));
    const auto b1 = encode_block_apps(enc, base, {3, 4, 5});
    EXPECT_EQ(base, 2u);
    uint16_t got0 = 0;
    uint16_t got1 = 0;
    int      idx = 0;
    int      k = 0;
    int      n = 0;
    int      sn = 0;
    ASSERT_TRUE(rs_block_erasure::unpack_header(b0[0].data(), b0[0].size(), &got0, &idx, &k, &n,
                                                &sn));
    ASSERT_TRUE(rs_block_erasure::unpack_header(b1[0].data(), b1[0].size(), &got1, &idx, &k, &n,
                                                &sn));
    EXPECT_EQ(got0, 0u);
    EXPECT_EQ(got1, 2u);
}

TEST(RsBlockErasureEmitTest, PeerRestartSilenceNoGapCounted)
{
    rs_block_erasure enc_a;
    rs_block_erasure enc_b;
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    ASSERT_TRUE(enc_a.init(2, 4, 20));
    ASSERT_TRUE(enc_b.init(2, 4, 20));
    vstreamer::fec_rx_payload_list out;
    feed_append(dec, encode_block_apps(enc_a, 0, {1, 2})[0], &out);
    feed_append(dec, encode_block_apps(enc_a, 2, {3, 4})[0], &out);
    clk.advance(300);
    feed_append(dec, encode_block_apps(enc_b, 5000, {5, 6})[0], &out);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u);
}

TEST(RsBlockErasureEmitTest, QuickForwardRestartNoPhantomLoss)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto block0 = encode_block_apps(enc, 0, {1, 2, 3, 4});
    const auto block_far = encode_block_apps(enc, 20000, {9, 10, 11, 12});
    vstreamer::fec_rx_payload_list discard;
    for (const auto &s : block0)
    {
        feed_append(dec, s, &discard);
    }
    vstreamer::fec_rx_payload_list out;
    for (const auto &s : block_far)
    {
        feed_append(dec, s, &out);
    }
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u);
    EXPECT_EQ(1u, dec.new_references());
    for (uint8_t tag : {9, 10, 11, 12})
    {
        EXPECT_TRUE(out_has_app_tag(out, tag));
    }
}

TEST(RsBlockErasureEmitTest, LargeButRealHoleStillCounted)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(6, 8, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto block0 = encode_block_apps(enc, 0, {1, 2, 3, 4, 5, 6});
    /* Within k_max_hi_block_id_deviation: a real hole in the same session. */
    const uint16_t far_base = static_cast<uint16_t>(6U + 1000U);
    const auto block_far = encode_block_apps(enc, far_base, {9, 10, 11, 12, 13, 14});
    vstreamer::fec_rx_payload_list discard;
    for (const auto &s : block0)
    {
        feed_append(dec, s, &discard);
    }
    for (const auto &s : block_far)
    {
        feed_append(dec, s, &discard);
    }
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list out;
    dec.poll_rx(&out);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 1000u);
    EXPECT_EQ(0u, dec.new_references());
}

TEST(RsBlockErasureEmitTest, SduSeqWrapNoLoss)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(1, 1, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    for (int i = 0; i < 8; i++)
    {
        feed_append(dec, encode_one(enc, static_cast<uint16_t>(65530 + i),
                                    static_cast<uint8_t>(i & 0xFF)),
                    &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    ASSERT_EQ(out.size(), 8u);
}

TEST(RsBlockErasureEmitTest, LateBlockAfterJumpNotUndone)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(2, 4, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto a = encode_block_apps(enc, 0, {1, 2});
    const auto c = encode_block_apps(enc, 4, {5, 6});
    vstreamer::fec_rx_payload_list out;
    for (const auto &s : a)
    {
        feed_append(dec, s, &out);
    }
    for (const auto &s : c)
    {
        feed_append(dec, s, &out);
    }
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list tick;
    dec.poll_rx(&tick);
    out.insert(out.end(), tick.begin(), tick.end());
    const uint64_t lost_before = dec.take_fail_lost_app_pkts();
    EXPECT_EQ(lost_before, 2u);
    const auto b = encode_block_apps(enc, 2, {3, 4});
    for (const auto &s : b)
    {
        feed_append(dec, s, &out);
    }
    EXPECT_GE(out.size(), 4u);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u);
}

TEST(RsBlockErasureEmitTest, HeadHoleJumpCountsPartialBlockOnce)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto block_a = encode_block_apps(enc, 0, {1, 2, 3, 4});
    const auto block_b = encode_block_apps(enc, 4, {11, 12, 13, 14});
    const auto block_c = encode_block_apps(enc, 8, {21, 22, 23, 24});
    vstreamer::fec_rx_payload_list out;
    vstreamer::fec_rx_payload_list discard;
    feed_append(dec, block_a[4], &discard);
    feed_append(dec, block_b[0], &discard);
    feed_append(dec, block_b[1], &discard);
    for (const auto &s : block_c)
    {
        feed_append(dec, s, &discard);
    }
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list step;
    dec.poll_rx(&step);
    out.insert(out.end(), step.begin(), step.end());
    clk.advance(k_past_deadline_ms);
    dec.poll_rx(&step);
    out.insert(out.end(), step.begin(), step.end());

    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 6u);
    ASSERT_GE(out.size(), 6u);
    EXPECT_EQ(out[0].u8()[0], 11);
    EXPECT_EQ(out[1].u8()[0], 12);
    EXPECT_EQ(out[2].u8()[0], 21);
    EXPECT_EQ(out[3].u8()[0], 22);
    EXPECT_EQ(out[4].u8()[0], 23);
    EXPECT_EQ(out[5].u8()[0], 24);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u);
}

namespace
{

/* Feeds one full block of a new sender session at `base`, tagged with `tag`. */
void feed_tagged_block(rs_block_erasure &enc, rs_block_erasure &dec, uint16_t base, uint8_t tag,
                       vstreamer::fec_rx_payload_list *out)
{
    for (const auto &s : encode_block_apps(enc, base, {tag, tag, tag, tag}))
    {
        feed_append(dec, s, out);
    }
}

std::vector<uint8_t> app_tags(const vstreamer::fec_rx_payload_list &rows)
{
    std::vector<uint8_t> tags;
    for (const auto &row : rows)
    {
        tags.push_back(row.empty() ? 0 : row.u8()[0]);
    }
    return tags;
}

/* Old session at old_base, then a new session at new_base with no silence in between: every
 * new-session SDU is delivered in order and nothing is counted lost. */
void expect_quick_restart_clean(uint16_t old_base, uint16_t new_base, int new_blocks)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    feed_tagged_block(enc, dec, old_base, 1, &out);
    feed_tagged_block(enc, dec, static_cast<uint16_t>(old_base + 4), 2, &out);
    std::vector<uint8_t> want = {1, 1, 1, 1, 2, 2, 2, 2};
    for (int b = 0; b < new_blocks; b++)
    {
        const uint8_t tag = static_cast<uint8_t>(10 + b);
        feed_tagged_block(enc, dec, static_cast<uint16_t>(new_base + 4 * b), tag, &out);
        want.insert(want.end(), 4, tag);
        clk.advance(5);
        vstreamer::fec_rx_payload_list step;
        dec.poll_rx(&step);
        out.insert(out.end(), step.begin(), step.end());
    }
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list step;
    dec.poll_rx(&step);
    out.insert(out.end(), step.begin(), step.end());
    EXPECT_EQ(app_tags(out), want) << "old " << old_base << " new " << new_base;
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u) << "old " << old_base << " new " << new_base;
}

}  // namespace

TEST(RsBlockErasureEmitTest, QuickForwardRestartKeepsLaterBlocks)
{
    expect_quick_restart_clean(0, 20000, 30);
}

TEST(RsBlockErasureEmitTest, QuickBackwardRestartResyncs)
{
    expect_quick_restart_clean(30000, 10000, 30);
}

TEST(RsBlockErasureEmitTest, QuickRestartRandomBases)
{
    std::mt19937 rng(12345);
    for (int i = 0; i < 12; i++)
    {
        const uint16_t old_base = static_cast<uint16_t>(rng());
        uint16_t       new_base = 0;
        do
        {
            new_base = static_cast<uint16_t>(rng());
        } while (std::abs(static_cast<int16_t>(static_cast<uint16_t>(new_base - old_base))) <=
                 2048 + 64);
        expect_quick_restart_clean(old_base, new_base, 20);
    }
}

/* A stray old-session shard arriving after a restart is more than k_max_lo_block_id_deviation
 * behind the new reference, so by the rule it is taken as a new reference too (its SDUs leave),
 * and the next new-session block takes the reference back. */
TEST(RsBlockErasureEmitTest, StrayOldShardAfterResyncTakesReference)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    feed_tagged_block(enc, dec, 0, 1, &out);
    const auto stray = encode_block_apps(enc, 4, {2, 2, 2, 2});
    std::vector<uint8_t> want = {1, 1, 1, 1};
    for (int b = 0; b < 30; b++)
    {
        const uint8_t tag = static_cast<uint8_t>(10 + b);
        feed_tagged_block(enc, dec, static_cast<uint16_t>(20000 + 4 * b), tag, &out);
        want.insert(want.end(), 4, tag);
        if (20 == b)
        {
            feed_append(dec, stray[0], &out);
            feed_append(dec, stray[1], &out);
            want.insert(want.end(), 2, 2);
        }
    }
    EXPECT_EQ(app_tags(out), want);
    EXPECT_EQ(3u, dec.new_references());
}

/* A head block given up with an SDU still missing is a failed block: the depayloader sees the
 * hole, so the failure counters must show it as well. */
TEST(RsBlockErasureEmitTest, HeadGiveUpWithMissingSduCountsAsFailure)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    const auto       air = encode_block_apps(enc, 10, {10, 11, 12, 13});
    vstreamer::fec_rx_payload_list out;
    for (size_t i = 0; i < air.size(); i++)
    {
        /* Lose data shard 2 and all parity: the block cannot be rebuilt. */
        if (i == 2 || fec_shard_index(air[i]) >= 4)
        {
            continue;
        }
        feed_append(dec, air[i], &out);
    }
    for (const auto &shard : encode_block_apps(enc, 14, {14, 15, 16, 17}))
    {
        feed_append(dec, shard, &out); /* the later block arrives complete */
    }

    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list tick;
    dec.poll_rx(&tick);
    out.insert(out.end(), tick.begin(), tick.end());
    ASSERT_TRUE(out_has_app_tag(out, 13));
    ASSERT_FALSE(out_has_app_tag(out, 12));

    EXPECT_EQ(1u, dec.decode_fail());
    EXPECT_EQ(1u, dec.given_up_blocks());
}

/* Giving up a head block that was delivered in full is not a failure. */
TEST(RsBlockErasureEmitTest, LosslessStreamHasNoFailures)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    for (uint16_t b = 0; b < 8; b++)
    {
        const uint16_t base = static_cast<uint16_t>(b * 4);
        const auto     air = encode_block_apps(
            enc, base,
            {static_cast<uint8_t>(base), static_cast<uint8_t>(base + 1),
             static_cast<uint8_t>(base + 2), static_cast<uint8_t>(base + 3)});
        for (const auto &shard : air)
        {
            feed_append(dec, shard, &out);
        }
    }
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list tick;
    dec.poll_rx(&tick);
    EXPECT_EQ(32u, out.size() + tick.size());
    EXPECT_EQ(0u, dec.decode_fail());
}

namespace
{

/* Blocks 0..blocks-1 of k=10/n=11 at sdu_base 10*b, tags 10*b + i + 1; each loses data shard 0
 * and its parity, so none can decode. Then one complete block after them. */
void feed_undecodable_run(rs_block_erasure &enc, rs_block_erasure &dec, int blocks,
                          vstreamer::fec_rx_payload_list *out)
{
    for (int b = 0; b <= blocks; b++)
    {
        std::vector<uint8_t> tags;
        for (int i = 0; i < 10; i++)
        {
            tags.push_back(static_cast<uint8_t>(10 * b + i + 1));
        }
        const auto air = encode_block_apps(enc, static_cast<uint16_t>(10 * b), tags);
        for (const auto &shard : air)
        {
            const int idx = fec_shard_index(shard);
            if (b < blocks && (0 == idx || 10 == idx))
            {
                continue;
            }
            feed_append(dec, shard, out);
        }
    }
}

void expect_ascending(const vstreamer::fec_rx_payload_list &out)
{
    for (size_t i = 1; i < out.size(); i++)
    {
        EXPECT_LT(out[i - 1].u8()[0], out[i].u8()[0]) << "emit #" << i;
    }
}

}  // namespace

/* Several undecodable blocks behind each other are flushed at their own deadlines in one pass,
 * in order, then the complete block after them is released. */
TEST(RsBlockErasureEmitTest, UndecodableRunFlushedAtDeadlines)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(10, 11, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    feed_undecodable_run(enc, dec, 4, &out);
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list tick;
    dec.poll_rx(&tick);
    out.insert(out.end(), tick.begin(), tick.end());
    EXPECT_EQ(4U, dec.given_up_blocks());
    EXPECT_EQ(0U, dec.evicted_blocks());
    EXPECT_EQ(4U * 9U + 10U, out.size());
    expect_ascending(out);
}

/* Blocks whose deadlines pass in the same poll are flushed in order from the head, so their
 * surviving payloads still leave in sequence. */
TEST(RsBlockErasureEmitTest, ExpiredBlocksEmitInOrder)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(10, 11, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    feed_undecodable_run(enc, dec, 6, &out);
    clk.advance(k_past_deadline_ms);
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    EXPECT_EQ(6U * 9U + 10U, out.size());
    expect_ascending(out);
}

namespace
{

/* Sends a k/n block the way stream_sender spreads it: air position p of c at t0 + p*spread/c.
 * `skip` lists shard indices lost on the way; `late_ms` delays shard index `late_idx`. */
void feed_spread_block(rs_block_erasure &enc, rs_block_erasure &dec, fake_clock &clk,
                       uint16_t base, int sdus, int spread_ms,
                       vstreamer::fec_rx_payload_list *out, std::vector<int> skip = {},
                       int late_idx = -1, int late_ms = 0)
{
    std::vector<uint8_t> tags;
    for (int i = 0; i < sdus; i++)
    {
        tags.push_back(static_cast<uint8_t>(base + i));
    }
    const auto air = encode_block_apps(enc, base, tags);
    const auto t0 = clk.t;
    std::vector<std::pair<std::chrono::steady_clock::time_point, size_t>> sched;
    for (size_t p = 0; p < air.size(); p++)
    {
        auto at = t0 + std::chrono::microseconds(static_cast<int64_t>(spread_ms) * 1000 *
                                                 static_cast<int64_t>(p) /
                                                 static_cast<int64_t>(air.size()));
        if (fec_shard_index(air[p]) == late_idx)
        {
            at += std::chrono::milliseconds(late_ms);
        }
        sched.emplace_back(at, p);
    }
    std::sort(sched.begin(), sched.end());
    for (const auto &[at, p] : sched)
    {
        if (std::find(skip.begin(), skip.end(), fec_shard_index(air[p])) != skip.end())
        {
            continue;
        }
        if (at > clk.t)
        {
            clk.t = at;
        }
        feed_append(dec, air[p], out);
    }
    clk.t = std::max(clk.t, t0 + std::chrono::milliseconds(spread_ms));
}

}  // namespace

/* The spread is estimated from receive times after two blocks, from all their shards. */
TEST(RsBlockErasureEmitTest, SpreadEstimatedFromArrivals)
{
    for (const int spread : {0, 20, 75, 150, 240})
    {
        rs_block_erasure enc;
        ASSERT_TRUE(enc.init(10, 14, 20));
        rs_block_erasure dec;
        fake_clock       clk;
        clk.attach(dec);
        vstreamer::fec_rx_payload_list out;
        EXPECT_FALSE(dec.have_spread_estimate());
        EXPECT_DOUBLE_EQ(dec.spread_estimate_ms(), rs_block_erasure::k_spread_cap_ms);
        feed_spread_block(enc, dec, clk, 0, 10, spread, &out, {3, 11});
        EXPECT_FALSE(dec.have_spread_estimate()) << spread;
        feed_spread_block(enc, dec, clk, 10, 10, spread, &out, {0, 12});
        ASSERT_TRUE(dec.have_spread_estimate()) << spread;
        EXPECT_NEAR(dec.spread_estimate_ms(), spread, 0.5) << spread;
        EXPECT_EQ(20u, out.size());
    }
}

/* One delayed shard does not move the estimate (median of pairwise slopes). */
TEST(RsBlockErasureEmitTest, SpreadEstimateRobustToOneLateShard)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(10, 14, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    feed_spread_block(enc, dec, clk, 0, 10, 70, &out, {}, 4, 40);
    feed_spread_block(enc, dec, clk, 10, 10, 70, &out, {}, 2, 40);
    ASSERT_TRUE(dec.have_spread_estimate());
    EXPECT_NEAR(dec.spread_estimate_ms(), 70, 3);
}

/* The estimate follows the sender when it changes its spread. */
TEST(RsBlockErasureEmitTest, SpreadEstimateTracksChanges)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(10, 14, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    uint16_t base = 0;
    for (int b = 0; b < 4; b++, base = static_cast<uint16_t>(base + 10))
    {
        feed_spread_block(enc, dec, clk, base, 10, 150, &out);
    }
    EXPECT_NEAR(dec.spread_estimate_ms(), 150, 1);
    for (int b = 0; b < 12; b++, base = static_cast<uint16_t>(base + 10))
    {
        feed_spread_block(enc, dec, clk, base, 10, 30, &out);
    }
    EXPECT_NEAR(dec.spread_estimate_ms(), 30, 5);
    EXPECT_EQ(16u * 10u, out.size());
}

/* Before an estimate the head waits up to the spread cap; with one, only the spread. */
TEST(RsBlockErasureEmitTest, DeadlineFollowsEstimate)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    {
        rs_block_erasure dec;
        fake_clock       clk;
        clk.attach(dec);
        vstreamer::fec_rx_payload_list out;
        const auto air = encode_block_apps(enc, 0, {1, 2, 3, 4});
        feed_append(dec, air[0], &out); /* only the first data shard */
        /* Last of 6 air positions due at 5/6 of the assumed spread, plus the margin. */
        const int due = rs_block_erasure::k_spread_cap_ms * 5 / 6 +
                        rs_block_erasure::k_deadline_margin_min_ms;
        clk.advance(due - 5);
        dec.poll_rx(&out);
        EXPECT_EQ(0u, dec.given_up_blocks());
        clk.advance(10);
        dec.poll_rx(&out);
        EXPECT_EQ(1u, dec.given_up_blocks());
    }
    {
        rs_block_erasure dec;
        fake_clock       clk;
        clk.attach(dec);
        vstreamer::fec_rx_payload_list out;
        feed_spread_block(enc, dec, clk, 0, 4, 40, &out);
        feed_spread_block(enc, dec, clk, 4, 4, 40, &out);
        ASSERT_TRUE(dec.have_spread_estimate());
        const auto air = encode_block_apps(enc, 8, {9, 10, 11, 12});
        feed_append(dec, air[0], &out);
        clk.advance(30);
        dec.poll_rx(&out);
        EXPECT_EQ(0u, dec.given_up_blocks()) << "flushed before the spread had passed";
        clk.advance(20);
        dec.poll_rx(&out);
        EXPECT_EQ(1u, dec.given_up_blocks()) << "still waiting for the spread cap";
    }
}

/* A block never seen at all is skipped once the next block shows the sender is past it. */
TEST(RsBlockErasureEmitTest, MissingBlockSkippedAfterNextBlockSpread)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    feed_spread_block(enc, dec, clk, 0, 4, 40, &out);
    feed_spread_block(enc, dec, clk, 4, 4, 40, &out);
    encode_block_apps(enc, 8, {0, 0, 0, 0}); /* block 8 lost entirely */
    feed_spread_block(enc, dec, clk, 12, 4, 40, &out);
    EXPECT_EQ(8u, out.size());
    clk.advance(50);
    vstreamer::fec_rx_payload_list tick;
    dec.poll_rx(&tick);
    EXPECT_EQ(4u, tick.size());
    EXPECT_EQ(4u, dec.take_fail_lost_app_pkts());
}

/* Behind by at most lo: ignored; behind by more, or ahead by more than hi: new reference. */
TEST(RsBlockErasureEmitTest, SequencerDeviationBounds)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(1, 1, 20));
    const int lo = rs_block_erasure::k_max_lo_block_id_deviation;
    const int hi = rs_block_erasure::k_max_hi_block_id_deviation;
    rs_block_erasure dec;
    vstreamer::fec_rx_payload_list out;
    const uint16_t ref = 30000;
    feed_append(dec, encode_one(enc, ref, 1), &out); /* reference -> 30001 */
    feed_append(dec, encode_one(enc, static_cast<uint16_t>(ref + 1 - lo), 2), &out);
    EXPECT_EQ(1u, dec.late_blocks());
    EXPECT_EQ(0u, dec.new_references());
    feed_append(dec, encode_one(enc, static_cast<uint16_t>(ref + 1 - lo - 1), 3), &out);
    EXPECT_EQ(1u, dec.new_references());
    const uint16_t r2 = static_cast<uint16_t>(ref + 1 - lo); /* next after the new block */
    feed_append(dec, encode_one(enc, static_cast<uint16_t>(r2 + hi), 4), &out);
    EXPECT_EQ(1u, dec.new_references()) << "ahead by hi is still the same session";
    feed_append(dec, encode_one(enc, static_cast<uint16_t>(r2 + hi + 1 + 1), 5), &out);
    EXPECT_EQ(2u, dec.new_references());
    EXPECT_TRUE(out_has_app_tag(out, 5));
    EXPECT_FALSE(out_has_app_tag(out, 2));
}

/* A later block that fails RS decode keeps its surviving messages in sequence. */
TEST(RsBlockErasureEmitTest, RsFailureOfLaterBlockStillInOrder)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    fake_clock       clk;
    clk.attach(dec);
    vstreamer::fec_rx_payload_list out;
    const auto a = encode_block_apps(enc, 0, {1, 2, 3, 4});
    const auto b = encode_block_apps(enc, 4, {5, 6, 7, 8});
    feed_append(dec, a[0], &out); /* head incomplete */
    for (size_t i = 0; i < b.size(); i++)
    {
        feed_append(dec, b[i], &out); /* b decodes and waits behind a */
    }
    EXPECT_EQ(1u, out.size());
    feed_append(dec, a[1], &out);
    feed_append(dec, a[2], &out);
    feed_append(dec, a[3], &out);
    ASSERT_EQ(8u, out.size());
    for (size_t i = 0; i < out.size(); i++)
    {
        EXPECT_EQ(out[i].u8()[0], i + 1);
    }
}
