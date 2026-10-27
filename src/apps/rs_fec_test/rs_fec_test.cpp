#include "core/rs_block_erasure.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

using vstreamer::fec_rx_payload_list;
using vstreamer::rs_block_erasure;

namespace
{

constexpr size_t k_max_datagram = 1476;

using wire_list = std::vector<std::vector<uint8_t>>;

/* App datagram tagged with its index (bytes 0..3, little endian). */
std::vector<uint8_t> app(uint32_t i, size_t len)
{
    std::vector<uint8_t> p(len < 4 ? 4 : len, static_cast<uint8_t>(i * 7));
    std::memcpy(p.data(), &i, 4);
    return p;
}

uint32_t tag_of(const bfc::shared_sized_buffer &p)
{
    uint32_t i = 0;
    if (p.size() >= 4)
    {
        std::memcpy(&i, p.u8(), 4);
    }
    return i;
}

/* Every shard the encoder sends for apps [first, first + count), in send order. */
wire_list encode(rs_block_erasure &enc, uint32_t first, uint32_t count, size_t len = 200)
{
    wire_list out;
    for (uint32_t i = first; i < first + count; i++)
    {
        const auto               a = app(i, len);
        rs_block_erasure::tx_out tx;
        enc.push_app(a.data(), a.size(), &tx);
        for (const auto &b : tx.shards)
        {
            out.emplace_back(b.u8(), b.u8() + b.size());
        }
    }
    return out;
}

void deliver(rs_block_erasure &dec, const std::vector<uint8_t> &w, fec_rx_payload_list *got)
{
    fec_rx_payload_list batch;
    dec.push_air(w.data(), w.size(), &batch);
    got->insert(got->end(), batch.begin(), batch.end());
}

/* got must hold tags first, first + 1, ... count of them. */
bool check_tags(const char *what, const fec_rx_payload_list &got, uint32_t first, size_t count)
{
    if (got.size() != count)
    {
        std::fprintf(stderr, "%s: expected %zu payloads got %zu\n", what, count, got.size());
        return false;
    }
    for (size_t i = 0; i < count; i++)
    {
        if (tag_of(got[i]) != first + i)
        {
            std::fprintf(stderr, "%s: payload %zu has tag %u\n", what, i, tag_of(got[i]));
            return false;
        }
    }
    return true;
}

}  // namespace

int main()
{
    /* RX deadlines run on this clock; nothing here sleeps. */
    auto now = std::chrono::steady_clock::time_point {} + std::chrono::hours(1);
    const auto clock = [&now] { return now; };
    constexpr int past_deadline_ms = rs_block_erasure::k_spread_cap_ms +
                                     rs_block_erasure::k_deadline_margin_min_ms + 10;

    /* Two lost data shards come back from parity, in order. */
    {
        rs_block_erasure enc;
        rs_block_erasure dec;
        if (!enc.init(10, 12, k_max_datagram))
        {
            std::fprintf(stderr, "init failed\n");
            return 1;
        }
        const wire_list w = encode(enc, 0, 10);
        if (w.size() != 12U)
        {
            std::fprintf(stderr, "expected 12 shards got %zu\n", w.size());
            return 1;
        }
        fec_rx_payload_list got;
        for (size_t i = 0; i < w.size(); i++)
        {
            if (2 != i && 5 != i)
            {
                deliver(dec, w[i], &got);
            }
        }
        if (!check_tags("recover", got, 0, 10) || dec.recovered() != 2U)
        {
            return 1;
        }
    }

    /* A later block's shards wait behind an earlier block's hole. */
    {
        rs_block_erasure enc;
        rs_block_erasure dec;
        enc.init(2, 4, k_max_datagram);
        const wire_list w = encode(enc, 0, 4); /* d0 d1 p0 p1 | d2 d3 p0 p1 */
        fec_rx_payload_list got;
        deliver(dec, w[0], &got);
        for (size_t i = 4; i < 8; i++)
        {
            deliver(dec, w[i], &got);
        }
        if (got.size() != 1U)
        {
            std::fprintf(stderr, "ooo: block1 released before block0 (%zu)\n", got.size());
            return 1;
        }
        deliver(dec, w[2], &got);
        if (!check_tags("ooo", got, 0, 4))
        {
            return 1;
        }
    }

    /* Many blocks, every shard: no failures, no recovery. */
    {
        rs_block_erasure enc;
        rs_block_erasure dec;
        enc.init(8, 12, k_max_datagram);
        fec_rx_payload_list got;
        for (const auto &s : encode(enc, 0, 800, 1000))
        {
            deliver(dec, s, &got);
        }
        if (!check_tags("lossless", got, 0, 800) || dec.decode_fail() != 0U ||
            dec.recovered() != 0U)
        {
            return 1;
        }
    }

    /* A block lost entirely (data and parity) is given up; later ones still come. */
    {
        rs_block_erasure enc;
        rs_block_erasure dec;
        dec.set_clock(clock);
        enc.init(4, 6, k_max_datagram);
        const wire_list w = encode(enc, 0, 12); /* 3 blocks of 6 shards */
        fec_rx_payload_list got;
        for (size_t i = 0; i < w.size(); i++)
        {
            if (i < 6 || i >= 12)
            {
                deliver(dec, w[i], &got);
            }
        }
        now += std::chrono::milliseconds(past_deadline_ms);
        fec_rx_payload_list tail;
        dec.poll_rx(&tail);
        got.insert(got.end(), tail.begin(), tail.end());
        if (got.size() != 8U || tag_of(got[4]) != 8U || dec.given_up_blocks() != 1U)
        {
            std::fprintf(stderr, "lost block: %zu payloads, given up %llu\n", got.size(),
                         static_cast<unsigned long long>(dec.given_up_blocks()));
            return 1;
        }
    }

    /* The data sequence keeps counting across a runtime k/n change. */
    {
        rs_block_erasure enc;
        rs_block_erasure dec;
        enc.init(4, 6, k_max_datagram);
        fec_rx_payload_list got;
        wire_list           w = encode(enc, 0, 6);
        rs_block_erasure::tx_out tail;
        enc.flush(&tail);
        for (const auto &b : tail.shards)
        {
            w.emplace_back(b.u8(), b.u8() + b.size());
        }
        enc.init(3, 5, k_max_datagram);
        const wire_list more = encode(enc, 6, 6);
        w.insert(w.end(), more.begin(), more.end());
        for (size_t i = 0; i < w.size(); i++)
        {
            if (i != 7 && i != 13) /* the short block's last data shard, a parity shard */
            {
                deliver(dec, w[i], &got);
            }
        }
        if (!check_tags("reinit", got, 0, 12))
        {
            return 1;
        }
    }

    /* A sender restart far outside the window takes a new reference. */
    {
        rs_block_erasure a;
        rs_block_erasure b;
        rs_block_erasure dec;
        a.init(4, 6, k_max_datagram);
        b.init(4, 6, k_max_datagram);
        a.set_tx_sequence(100);
        b.set_tx_sequence(40000);
        fec_rx_payload_list got;
        for (const auto &s : encode(a, 0, 8))
        {
            deliver(dec, s, &got);
        }
        for (const auto &s : encode(b, 8, 8))
        {
            deliver(dec, s, &got);
        }
        if (!check_tags("restart", got, 0, 16) || dec.new_references() != 1U)
        {
            return 1;
        }
    }

    /* n == k: data shards only. */
    {
        rs_block_erasure enc;
        rs_block_erasure dec;
        enc.init(5, 5, k_max_datagram);
        const wire_list w = encode(enc, 0, 20);
        fec_rx_payload_list got;
        for (const auto &s : w)
        {
            deliver(dec, s, &got);
        }
        if (w.size() != 20U || !check_tags("n==k", got, 0, 20))
        {
            return 1;
        }
    }

    /* u16 sequence wrap: lossless and in order, with recovery across it. */
    {
        rs_block_erasure enc;
        rs_block_erasure dec;
        enc.init(6, 8, k_max_datagram);
        enc.set_tx_sequence(65530);
        const wire_list w = encode(enc, 0, 24);
        fec_rx_payload_list got;
        for (size_t i = 0; i < w.size(); i++)
        {
            if (i != 8 && i != 9) /* the first two data shards after the wrap */
            {
                deliver(dec, w[i], &got);
            }
        }
        if (!check_tags("wrap", got, 0, 24))
        {
            return 1;
        }
    }

    std::printf("rs_fec_test: ok (%s)\n", rs_block_erasure().impl_name());
    return 0;
}
