/* rs_block_erasure receive side: sequencer, spread estimate and deadlines (see the header). */
#include "core/rs_block_erasure.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace
{

using vstreamer::rs_block_erasure;
using clock_type = std::chrono::steady_clock;

uint16_t load_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

double median(std::vector<double>* v)
{
    const size_t mid = v->size() / 2;
    std::nth_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(mid), v->end());
    const double hi = (*v)[mid];
    if (v->size() % 2 != 0)
    {
        return hi;
    }
    const double lo = *std::max_element(v->begin(), v->begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lo + hi);
}

}  // namespace

std::chrono::steady_clock::time_point vstreamer::rs_block_erasure::now() const
{
    return clock ? clock() : clock_type::now();
}

int vstreamer::rs_block_erasure::ring_dist(uint16_t a, uint16_t b)
{
    return static_cast<int>(static_cast<int16_t>(static_cast<uint16_t>(a - b)));
}

int vstreamer::rs_block_erasure::dist_from_emit(uint16_t sdu_base) const
{
    return ring_dist(sdu_base, emit_next);
}

void vstreamer::rs_block_erasure::note_head_hole_sdus(int gap_sdus)
{
    if (gap_sdus > 0)
    {
        fail_lost_app_pkts_count += static_cast<uint64_t>(gap_sdus);
    }
}

void vstreamer::rs_block_erasure::emit_payload(fec_rx_payload_list* out,
                                               bfc::shared_sized_buffer&& app)
{
    if (nullptr == out || app.empty())
    {
        return;
    }
    out->push_back(std::move(app));
}

bool vstreamer::rs_block_erasure::frag_to_app(const bfc::shared_sized_buffer& shard,
                                              bfc::shared_sized_buffer* app)
{
    if (nullptr == app || shard.size() < k_header_len + k_len_prefix)
    {
        return false;
    }
    const uint8_t* body = shard.u8() + k_header_len;
    const size_t   body_len = shard.size() - k_header_len;
    const uint16_t orig_len = load_be16(body);
    if (0 == orig_len)
    {
        app->clear();
        return true;
    }
    if (k_len_prefix + orig_len > body_len)
    {
        return false;
    }
    *app = shard.subview(k_header_len + k_len_prefix, orig_len);
    return !app->empty();
}

void vstreamer::rs_block_erasure::account_missing_shards(const rx_block_s& block)
{
    const int sn = block.sdu_n > 0 ? block.sdu_n : block.k;
    if (sn <= 0 || block.n <= 0 || block.k <= 0)
    {
        return;
    }
    const int expected = sn + (block.n - block.k);
    const int got = static_cast<int>(block.frags.size());
    if (expected > got)
    {
        missing_shards_count += static_cast<uint64_t>(expected - got);
    }
}

void vstreamer::rs_block_erasure::note_rx_block_output_shortfall(int expected, int available)
{
    if (expected > available)
    {
        fail_lost_app_pkts_count += static_cast<uint64_t>(expected - available);
    }
}

int vstreamer::rs_block_erasure::expected_sdus(const rx_block_s& block)
{
    return block.sdu_n > 0 ? block.sdu_n : block.k;
}

/* --- spread estimate and deadlines --- */

/* The sender puts data shards 0..sdu_n-1 on air first (empty tail slots are not sent), then
 * parity k..n-1, and releases position p of c at t0 + p * spread / c. */
int vstreamer::rs_block_erasure::air_pos(int index, int k, int sdu_n)
{
    return index < k ? index : sdu_n + (index - k);
}

int vstreamer::rs_block_erasure::air_count(int k, int n, int sdu_n)
{
    return sdu_n + (n - k);
}

std::chrono::nanoseconds vstreamer::rs_block_erasure::spread_now() const
{
    const double s = have_spread_estimate() ? spread_est_s : k_spread_cap_ms / 1000.0;
    return std::chrono::nanoseconds(static_cast<int64_t>(s * 1e9));
}

std::chrono::nanoseconds vstreamer::rs_block_erasure::margin_now() const
{
    const double floor_s = k_deadline_margin_min_ms / 1000.0;
    const double s = have_spread_estimate() ? std::max(floor_s, 3.0 * jitter_est_s) : floor_s;
    return std::chrono::nanoseconds(static_cast<int64_t>(s * 1e9));
}

double vstreamer::rs_block_erasure::spread_estimate_ms() const
{
    return (have_spread_estimate() ? spread_est_s : k_spread_cap_ms / 1000.0) * 1000.0;
}

double vstreamer::rs_block_erasure::jitter_estimate_ms() const
{
    return have_spread_estimate() ? jitter_est_s * 1000.0 : 0.;
}

/* Earliest start implied by any received shard: the least delayed one bounds when the block
 * left the sender. */
std::chrono::steady_clock::time_point vstreamer::rs_block_erasure::block_start(
    const rx_block_s& block) const
{
    const int  count = std::max(1, air_count(block.k, block.n, block.sdu_n));
    const auto spread = spread_now();
    auto       start = clock_type::time_point::max();
    for (const auto& [pos, at] : block.arrivals)
    {
        start = std::min(start, at - spread * pos / count);
    }
    return block.arrivals.empty() ? block.last_seen : start;
}

std::chrono::steady_clock::time_point vstreamer::rs_block_erasure::block_deadline(
    const rx_block_s& block) const
{
    const int count = std::max(1, air_count(block.k, block.n, block.sdu_n));
    return block_start(block) + spread_now() * (count - 1) / count + margin_now();
}

/* Theil-Sen over the block's arrivals: the median of the pairwise slopes is the per-position
 * interval (one late shard cannot skew it), spread = interval * count. The jitter is the
 * largest residual around the fitted line. Blocks are weighted by how many shards they gave. */
void vstreamer::rs_block_erasure::learn_spread(const rx_block_s& block)
{
    const auto& a = block.arrivals;
    if (a.size() < 2)
    {
        return;
    }
    const int count = air_count(block.k, block.n, block.sdu_n);
    if (count < 2)
    {
        return;
    }
    const auto t0 = a.front().second;
    const auto sec = [t0](clock_type::time_point t)
    { return std::chrono::duration<double>(t - t0).count(); };
    std::vector<double> slopes;
    slopes.reserve(a.size() * (a.size() - 1) / 2);
    for (size_t i = 0; i < a.size(); i++)
    {
        for (size_t j = i + 1; j < a.size(); j++)
        {
            if (a[i].first != a[j].first)
            {
                slopes.push_back((sec(a[j].second) - sec(a[i].second)) /
                                 static_cast<double>(a[j].first - a[i].first));
            }
        }
    }
    if (slopes.empty())
    {
        return;
    }
    const double        b = std::max(0., median(&slopes));
    std::vector<double> icepts;
    icepts.reserve(a.size());
    for (const auto& [pos, at] : a)
    {
        icepts.push_back(sec(at) - b * pos);
    }
    const double icept = median(&icepts);
    double       resid = 0.;
    for (const auto& [pos, at] : a)
    {
        resid = std::max(resid, std::fabs(sec(at) - (icept + b * pos)));
    }
    const double cap = k_spread_cap_ms / 1000.0;
    const double spread = std::min(cap, b * count);
    const double weight = static_cast<double>(a.size()) / static_cast<double>(count);
    if (0 == fitted_blocks)
    {
        spread_est_s = spread;
        jitter_est_s = resid;
    }
    else
    {
        /* The second fit counts as much as the first; later ones move the estimate by up to
         * half their difference, scaled by how many of the block's shards they saw. */
        const double alpha = 1 == fitted_blocks ? 0.5 * weight + 0.25 : 0.5 * weight;
        spread_est_s += alpha * (spread - spread_est_s);
        jitter_est_s += alpha * (resid - jitter_est_s);
    }
    fitted_blocks++;
}

/* --- sequencer --- */

void vstreamer::rs_block_erasure::new_reference(uint16_t sdu_base)
{
    evicted_blocks_count += rx_blocks.size();
    for (const auto& kv : rx_blocks)
    {
        account_missing_shards(kv.second);
    }
    rx_blocks.clear();
    ready_blocks.clear();
    emit_next = sdu_base;
    new_references_count++;
}

void vstreamer::rs_block_erasure::advance_head(int sdu_n)
{
    emit_next = static_cast<uint16_t>(emit_next + static_cast<uint16_t>(sdu_n));
}

void vstreamer::rs_block_erasure::try_stream_head_systematic(rx_block_s& block,
                                                             fec_rx_payload_list* out)
{
    const int sn = expected_sdus(block);
    while (block.released < sn)
    {
        const auto it = block.frags.find(block.released);
        if (it == block.frags.end())
        {
            break;
        }
        bfc::shared_sized_buffer app;
        if (!frag_to_app(it->second, &app))
        {
            break;
        }
        emit_payload(out, std::move(app));
        block.released++;
    }
}

int vstreamer::rs_block_erasure::abandon_partial_block(const rx_block_s& block,
                                                       fec_rx_payload_list* out)
{
    account_missing_shards(block);
    const int sn = expected_sdus(block);
    int       emitted = 0;
    for (int i = block.released; i < sn; i++)
    {
        const auto it = block.frags.find(i);
        if (it == block.frags.end())
        {
            continue;
        }
        bfc::shared_sized_buffer app;
        if (!frag_to_app(it->second, &app))
        {
            continue;
        }
        emit_payload(out, std::move(app));
        emitted++;
    }
    note_rx_block_output_shortfall(sn, block.released + emitted);
    return std::max(0, sn - (block.released + emitted));
}

/* Releases everything that can go out in sequence, flushing the head block at its deadline
 * and skipping a run of ids never seen once the next block shows the sender is past them. */
void vstreamer::rs_block_erasure::run_emit_engine(fec_rx_payload_list* out)
{
    if (!emit_base_set)
    {
        return;
    }
    const auto t = now();
    for (;;)
    {
        if (auto rit = ready_blocks.find(emit_next); rit != ready_blocks.end())
        {
            ready_block_s& rb = rit->second;
            for (int i = rb.released; i < rb.sdu_n && i < static_cast<int>(rb.payloads.size());
                 i++)
            {
                emit_payload(out, std::move(rb.payloads[static_cast<size_t>(i)]));
            }
            const int sn = rb.sdu_n;
            ready_blocks.erase(rit);
            advance_head(sn);
            continue;
        }
        if (auto rxit = rx_blocks.find(emit_next); rxit != rx_blocks.end())
        {
            try_stream_head_systematic(rxit->second, out);
            if (t < block_deadline(rxit->second))
            {
                break;
            }
            const rx_block_s snap = std::move(rxit->second);
            rx_blocks.erase(rxit);
            learn_spread(snap);
            if (abandon_partial_block(snap, out) > 0)
            {
                given_up_blocks_count++;
            }
            advance_head(expected_sdus(snap));
            continue;
        }
        /* No shard of the head id yet: find the nearest later block. */
        int                       best = 0;
        bool                      found = false;
        clock_type::time_point    next_start{};
        for (const auto& kv : rx_blocks)
        {
            const int d = dist_from_emit(kv.first);
            if (d > 0 && (!found || d < best))
            {
                found = true;
                best = d;
                next_start = block_start(kv.second);
            }
        }
        for (const auto& kv : ready_blocks)
        {
            const int d = dist_from_emit(kv.first);
            if (d > 0 && (!found || d < best))
            {
                found = true;
                best = d;
                next_start = kv.second.start;
            }
        }
        /* The missing blocks were closed before that one, so their last shard was due within
         * one spread of its start. */
        if (!found || t < next_start + spread_now() + margin_now())
        {
            break;
        }
        note_head_hole_sdus(best);
        emit_next = static_cast<uint16_t>(emit_next + static_cast<uint16_t>(best));
    }
}

bool vstreamer::rs_block_erasure::try_decode_block(uint16_t sdu_base, rx_block_s* block,
                                                   fec_rx_payload_list* out)
{
    if (nullptr == block)
    {
        return false;
    }
    if (static_cast<int>(block->frags.size()) < block->sdu_n)
    {
        return false;
    }
    size_t shard_slots = block->frags.size();
    for (int i = block->sdu_n; i < block->k; i++)
    {
        if (block->frags.find(i) == block->frags.end())
        {
            shard_slots++;
        }
    }
    if (shard_slots < static_cast<size_t>(block->k))
    {
        return false;
    }
    int                 rec = 0;
    fec_rx_payload_list decoded;
    const bool ok = decode_block(block->k, block->n, block->sdu_n, block->frags, &decoded, &rec);
    const rx_block_s snap = std::move(*block);
    rx_blocks.erase(sdu_base);
    learn_spread(snap);
    const int sn = expected_sdus(snap);
    ready_block_s ready;
    ready.released = snap.released;
    ready.sdu_n = sn;
    ready.start = block_start(snap);
    if (ok)
    {
        recovered_count += static_cast<uint64_t>(rec);
        blocks_count++;
        note_rx_block_output_shortfall(sn, static_cast<int>(decoded.size()));
        ready.payloads = std::move(decoded);
    }
    else
    {
        /* Keep what arrived, in place, so it still leaves in sequence. */
        rs_failures_count++;
        account_missing_shards(snap);
        ready.payloads.resize(static_cast<size_t>(sn));
        int have = snap.released;
        for (int i = snap.released; i < sn; i++)
        {
            const auto it = snap.frags.find(i);
            bfc::shared_sized_buffer app;
            if (it != snap.frags.end() && frag_to_app(it->second, &app))
            {
                ready.payloads[static_cast<size_t>(i)] = std::move(app);
                have++;
            }
        }
        note_rx_block_output_shortfall(sn, have);
    }
    ready_blocks[sdu_base] = std::move(ready);
    run_emit_engine(out);
    return true;
}

void vstreamer::rs_block_erasure::poll_rx(fec_rx_payload_list* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    run_emit_engine(out);
}

void vstreamer::rs_block_erasure::push_air(const uint8_t* data, size_t len,
                                           fec_rx_payload_list* out)
{
    if (nullptr == data || 0 == len)
    {
        return;
    }
    push_air(bfc::shared_sized_buffer::copy_from(data, len), out);
}

void vstreamer::rs_block_erasure::push_air(bfc::shared_sized_buffer shard,
                                           fec_rx_payload_list* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (out == nullptr || shard.empty())
    {
        return;
    }
    // RX is always FEC-aware: k/n come from the shard header. Encode still requires init().
    uint16_t sdu_base = 0;
    int      index = 0;
    int      k = 0;
    int      n = 0;
    int      sdu_n = 0;
    if (shard.size() < k_header_len ||
        !unpack_header(shard.u8(), shard.size(), &sdu_base, &index, &k, &n, &sdu_n))
    {
        hdr_errors_count++;
        return;
    }
    const auto t = now();
    if (!emit_base_set)
    {
        emit_base_set = true;
        emit_next = sdu_base;
    }
    const int d = dist_from_emit(sdu_base);
    if ((d < 0 && -d > k_max_lo_block_id_deviation) || d > k_max_hi_block_id_deviation)
    {
        new_reference(sdu_base);
    }
    else if (d < 0)
    {
        late_blocks_count++;
        run_emit_engine(out);
        return;
    }
    /* Already decoded and waiting for the head: a further (parity) shard is redundant. */
    if (ready_blocks.find(sdu_base) != ready_blocks.end())
    {
        run_emit_engine(out);
        return;
    }

    auto it = rx_blocks.find(sdu_base);
    if (it == rx_blocks.end())
    {
        if (rx_blocks.size() >= k_block_max)
        {
            /* Memory bound: drop the block furthest ahead (it is the last to be released). */
            auto far = rx_blocks.begin();
            for (auto j = rx_blocks.begin(); j != rx_blocks.end(); ++j)
            {
                if (dist_from_emit(j->first) > dist_from_emit(far->first))
                {
                    far = j;
                }
            }
            evicted_blocks_count++;
            account_missing_shards(far->second);
            rx_blocks.erase(far);
        }
        rx_block_s nb;
        nb.k = k;
        nb.n = n;
        nb.sdu_n = sdu_n;
        it = rx_blocks.emplace(sdu_base, std::move(nb)).first;
    }
    else if (it->second.k != k || it->second.n != n || it->second.sdu_n != sdu_n)
    {
        kn_mismatch_count++;
        rx_blocks.erase(it);
        run_emit_engine(out);
        return;
    }

    rx_block_s* block = &it->second;
    if (block->frags.count(index) != 0)
    {
        run_emit_engine(out);
        return;
    }
    block->frags.emplace(index, std::move(shard));
    block->arrivals.emplace_back(air_pos(index, k, sdu_n), t);
    block->last_seen = t;

    if (sdu_base == emit_next)
    {
        try_stream_head_systematic(*block, out);
    }
    if (try_decode_block(sdu_base, block, out))
    {
        return;
    }
    run_emit_engine(out);
}
