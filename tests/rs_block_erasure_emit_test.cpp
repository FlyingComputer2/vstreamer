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

std::vector<uint8_t> encode_one(rs_block_erasure &enc, uint16_t block_id, uint8_t tag)
{
    const std::vector<uint8_t> app = {tag};
    std::vector<std::vector<uint8_t>> air;
    EXPECT_TRUE(enc.encode_block({app}, block_id, &air));
    EXPECT_EQ(air.size(), 1u);
    return air[0];
}

std::vector<std::vector<uint8_t>> encode_block_apps(rs_block_erasure &enc, uint16_t block_id,
                                                    const std::vector<uint8_t> &tags)
{
    std::vector<std::vector<uint8_t>> apps;
    for (uint8_t t : tags)
    {
        apps.push_back({t});
    }
    std::vector<std::vector<uint8_t>> air;
    EXPECT_TRUE(enc.encode_block(apps, block_id, &air));
    return air;
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
    for (int i = 0; i < 70; i++)
    {
        feed_append(dec, encode_one(enc_fill, static_cast<uint16_t>(10 + i),
                                    static_cast<uint8_t>(i & 0xFF)),
                    &out);
    }
    EXPECT_GT(dec.evicted_blocks(), 0u);
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
    EXPECT_FALSE(enc.init(6, 20, 20));
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
        const int idx = static_cast<int>(air[i][1] & rs_block_erasure::k_wire_index_mask);
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
        const int idx = static_cast<int>(air[i][1] & rs_block_erasure::k_wire_index_mask);
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

/* Restarted peer whose random start id lands far *ahead* of emit_next (C3 resync, forward).
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
        for (int i = 0; i < 67; i++)
        {
            for (const auto &shard :
                 encode_block_apps(enc_a, static_cast<uint16_t>(i), {1, 1, 1, 1, 1, 1}))
            {
                feed_append(dec, shard, &out);
            }
        }
        ASSERT_EQ(out.size(), 67u * 6u);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        const int start = (66 + offset) & 0xFF;
        for (int i = 0; i < 20; i++)
        {
            const int     id = (start + i) & 0xFF;
            const uint8_t tag = static_cast<uint8_t>(100 + i);
            vstreamer::fec_rx_payload_list step;
            for (const auto &shard : encode_block_apps(enc_b, static_cast<uint16_t>(id),
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
    for (int i = 0; i < 70; i++)
    {
        feed_append(dec, encode_one(enc_fill, static_cast<uint16_t>(10 + i),
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


