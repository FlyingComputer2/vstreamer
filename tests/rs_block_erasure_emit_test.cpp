#include "core/rs_block_erasure.hpp"

#include <gtest/gtest.h>

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
    EXPECT_FALSE(enc.init(256, 256, 20));
    EXPECT_TRUE(enc.enabled());
    EXPECT_EQ(enc.k(), 6);
    EXPECT_EQ(enc.n(), 8);
}

TEST(RsBlockErasureEmitTest, WrapLateBlockDeliveredImmediately)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(1, 1, 20));
    rs_block_erasure dec;

    vstreamer::fec_rx_payload_list all;
    for (uint16_t id = 250; id <= 253; ++id)
    {
        feed_append(dec, encode_one(enc, id, static_cast<uint8_t>(id)), &all);
    }
    feed_append(dec, encode_one(enc, 255, 255), &all);
    feed_append(dec, encode_one(enc, 0, 0), &all);
    feed_append(dec, encode_one(enc, 1, 1), &all);
    feed_append(dec, encode_one(enc, 2, 2), &all);

    const std::vector<uint8_t> late_shard = encode_one(enc, 254, 254);
    vstreamer::fec_rx_payload_list late_step;
    dec.push_air(late_shard.data(), late_shard.size(), &late_step);
    EXPECT_TRUE(out_has_app_tag(late_step, 254));
}

TEST(RsBlockErasureEmitTest, WrapInOrderNoLoss)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(1, 1, 20));
    rs_block_erasure dec;
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

    /* Later block data starts emit_hold timer (plan rule 5a). */
    const auto later = encode_block_apps(enc, 11, {14, 15, 16, 17});
    feed_append(dec, later[0], &out);
    EXPECT_FALSE(out_has_app_tag(out, 14));

    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.emit_hold_ms() + 5)));
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

TEST(RsBlockErasureEmitTest, PeerRestartResync)
{
    rs_block_erasure dec;
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
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (int i = 0; i < 10; i++)
    {
        feed_append(dec, encode_one(enc_b, static_cast<uint16_t>(250 + i),
                                    static_cast<uint8_t>((250 + i) & 0xFF)),
                    &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    EXPECT_GE(out.size(), 310u);
}

/* Restarted peer whose random start id lands far *ahead* of emit_next (resync, forward).
 * The new blocks must be delivered on arrival: holding them for emit_hold_ms while the ids
 * cross the half ring made the backward rebase discard them (loopback SenderRestartMidStream). */
TEST(RsBlockErasureEmitTest, PeerRestartResyncForwardJump)
{
    for (const int offset : {10, 80, 120})
    {
        rs_block_erasure dec;
        rs_block_erasure enc_a;
        rs_block_erasure enc_b;
        ASSERT_TRUE(enc_a.init(6, 8, 20));
        ASSERT_TRUE(enc_b.init(6, 8, 20));

        vstreamer::fec_rx_payload_list out;
        uint16_t                       tx_base = 0;
        for (int i = 0; i < 67; i++)
        {
            for (const auto &shard :
                 encode_block_apps(enc_a, tx_base, {1, 1, 1, 1, 1, 1}))
            {
                feed_append(dec, shard, &out);
            }
            tx_base = static_cast<uint16_t>(tx_base + 6);
        }
        ASSERT_EQ(out.size(), 67u * 6u);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        const uint16_t start = static_cast<uint16_t>(66 * 6 + offset);
        for (int i = 0; i < 20; i++)
        {
            const uint16_t id = static_cast<uint16_t>(start + static_cast<uint16_t>(i * 6));
            const uint8_t  tag = static_cast<uint8_t>(100 + i);
            vstreamer::fec_rx_payload_list step;
            for (const auto &shard : encode_block_apps(enc_b, id,
                                                       {tag, tag, tag, tag, tag, tag}))
            {
                feed_append(dec, shard, &step);
            }
            ASSERT_EQ(step.size(), 6u) << "offset " << offset << ": restart block " << i
                                       << " (id " << id << ") not delivered on arrival";
            for (const auto &row : step)
            {
                EXPECT_EQ(row.u8()[0], tag);
            }
        }
    }
}

TEST(RsBlockErasureEmitTest, RingEvictsStalePartial)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure enc_fill;
    ASSERT_TRUE(enc_fill.init(1, 1, 20));
    rs_block_erasure dec;
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
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        vstreamer::fec_rx_payload_list flush;
        dec.poll_rx(&flush);
        out.insert(out.end(), flush.begin(), flush.end());
    }
    EXPECT_EQ(5U, out.size());
    EXPECT_EQ(3U, dec.take_fail_lost_app_pkts());
    EXPECT_EQ(1U, dec.take_evicted_blocks()); /* block 0 expires; no phantom block 1 */
}

TEST(RsBlockErasureEmitTest, Kn32RoundTripMaxErasures)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(32, 32, 20));
    rs_block_erasure dec;
    std::vector<uint8_t> tags;
    for (int i = 0; i < 32; i++)
    {
        tags.push_back(static_cast<uint8_t>(i));
    }
    const auto air = encode_block_apps(enc, 1001, tags);
    ASSERT_EQ(air.size(), 32u);
    vstreamer::fec_rx_payload_list out;
    for (const auto &shard : air)
    {
        feed_append(dec, shard, &out);
    }
    vstreamer::fec_rx_payload_list flush;
    dec.poll_rx(&flush);
    out.insert(out.end(), flush.begin(), flush.end());
    ASSERT_EQ(out.size(), 32u);
}

TEST(RsBlockErasureEmitTest, Kn31RoundTripMaxErasures)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(31, 31, 20));
    rs_block_erasure dec;
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
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.emit_hold_ms() + 10)));
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
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.emit_hold_ms() + 10)));
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
    ASSERT_TRUE(enc_a.init(2, 4, 20));
    ASSERT_TRUE(enc_b.init(2, 4, 20));
    vstreamer::fec_rx_payload_list out;
    feed_append(dec, encode_block_apps(enc_a, 0, {1, 2})[0], &out);
    feed_append(dec, encode_block_apps(enc_a, 2, {3, 4})[0], &out);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    feed_append(dec, encode_block_apps(enc_b, 5000, {5, 6})[0], &out);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u);
}

TEST(RsBlockErasureEmitTest, QuickForwardRestartNoPhantomLoss)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
    const auto block0 = encode_block_apps(enc, 0, {1, 2, 3, 4});
    const auto block_far = encode_block_apps(enc, 20000, {9, 10, 11, 12});
    vstreamer::fec_rx_payload_list discard;
    for (const auto &s : block0)
    {
        feed_append(dec, s, &discard);
    }
    for (const auto &s : block_far)
    {
        feed_append(dec, s, &discard);
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.emit_hold_ms() + 5)));
    vstreamer::fec_rx_payload_list out;
    dec.poll_rx(&out);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u);
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
    const auto block0 = encode_block_apps(enc, 0, {1, 2, 3, 4, 5, 6});
    const uint16_t far_base = static_cast<uint16_t>(6U + 2042U);
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
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.emit_hold_ms() + 5)));
    vstreamer::fec_rx_payload_list out;
    dec.poll_rx(&out);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 2042u);
}

TEST(RsBlockErasureEmitTest, SduSeqWrapNoLoss)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(1, 1, 20));
    rs_block_erasure dec;
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
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.emit_hold_ms() + 10)));
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
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.emit_hold_ms() + 5)));
    vstreamer::fec_rx_payload_list step;
    dec.poll_rx(&step);
    out.insert(out.end(), step.begin(), step.end());
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.rx_hold_ms() + 5)));
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
    vstreamer::fec_rx_payload_list out;
    feed_tagged_block(enc, dec, old_base, 1, &out);
    feed_tagged_block(enc, dec, static_cast<uint16_t>(old_base + 4), 2, &out);
    std::vector<uint8_t> want = {1, 1, 1, 1, 2, 2, 2, 2};
    for (int b = 0; b < new_blocks; b++)
    {
        const uint8_t tag = static_cast<uint8_t>(10 + b);
        feed_tagged_block(enc, dec, static_cast<uint16_t>(new_base + 4 * b), tag, &out);
        want.insert(want.end(), 4, tag);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        vstreamer::fec_rx_payload_list step;
        dec.poll_rx(&step);
        out.insert(out.end(), step.begin(), step.end());
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<unsigned>(dec.emit_hold_ms() + 5)));
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

TEST(RsBlockErasureEmitTest, StrayOldShardAfterResyncIgnored)
{
    rs_block_erasure enc;
    ASSERT_TRUE(enc.init(4, 6, 20));
    rs_block_erasure dec;
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
            /* Late shards of the old session, reordered past the resync. */
            feed_append(dec, stray[0], &out);
            feed_append(dec, stray[1], &out);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        vstreamer::fec_rx_payload_list step;
        dec.poll_rx(&step);
        out.insert(out.end(), step.begin(), step.end());
    }
    EXPECT_EQ(app_tags(out), want);
    EXPECT_EQ(dec.take_fail_lost_app_pkts(), 0u);
}
