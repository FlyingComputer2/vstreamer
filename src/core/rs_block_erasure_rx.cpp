/* rs_block_erasure receive side: in-order release, block decode, spread estimate and give-up. */
#include "core/rs_block_erasure.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace
{

using vstreamer::rs_block_erasure;
using clock_type = std::chrono::steady_clock;

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

std::chrono::nanoseconds seconds_ns(double s)
{
    return std::chrono::nanoseconds(static_cast<int64_t>(s * 1e9));
}

double seconds(clock_type::duration d)
{
    return std::chrono::duration<double>(d).count();
}

/* How far past a block's data the receiver looks for the next received data shard. */
constexpr int k_close_scan = 64;

/*
 * When the last parity of a block is due, after its close. The sender releases parity row j of a
 * block of c = k_total + n shards at max(close + j*g, first + (k_total + j)*g), g = spread / c
 * (stream_sender), so a block that filled in a burst sends its parity over about the whole
 * spread, and one that filled slowly sends it right after the close.
 */
double last_parity_after_close(double fill_s, int k_total, int n, double spread_s)
{
    const double g = spread_s / static_cast<double>(k_total + n);
    return std::max(g * (n - 1), g * (k_total + n - 1) - fill_s);
}

}  // namespace

std::chrono::steady_clock::time_point vstreamer::rs_block_erasure::now() const
{
    return clock ? clock() : clock_type::now();
}

int vstreamer::rs_block_erasure::seq_dist(uint16_t a, uint16_t b)
{
    return static_cast<int>(static_cast<int16_t>(static_cast<uint16_t>(a - b)));
}

vstreamer::rs_block_erasure::rx_slot* vstreamer::rs_block_erasure::slot_for(uint16_t seq)
{
    return &ring[seq & (k_rx_ring - 1)];
}

/* --- spread estimate --- */

std::chrono::nanoseconds vstreamer::rs_block_erasure::margin_now() const
{
    const double floor_s = k_deadline_margin_min_ms / 1000.0;
    return seconds_ns(have_spread_estimate() ? std::max(floor_s, 3.0 * jitter_est_s) : floor_s);
}

double vstreamer::rs_block_erasure::spread_estimate_ms() const
{
    return (have_spread_estimate() ? spread_est_s : k_spread_cap_ms / 1000.0) * 1000.0;
}

double vstreamer::rs_block_erasure::jitter_estimate_ms() const
{
    return have_spread_estimate() ? jitter_est_s * 1000.0 : 0.;
}

/* The block closed when its last data shard was pushed: that shard's arrival, else the first
 * later data shard received (pushed after the close), else its first parity. */
bool vstreamer::rs_block_erasure::close_time(uint16_t base, int k_total,
                                             std::chrono::steady_clock::time_point* at) const
{
    bool                   found = false;
    clock_type::time_point best {};
    const auto             consider = [&](clock_type::time_point t)
    {
        if (!found || t < best)
        {
            best = t;
            found = true;
        }
    };
    for (int j = k_total - 1; j < k_total + k_close_scan; j++)
    {
        const uint16_t seq = static_cast<uint16_t>(base + j);
        if (seq_dist(seq, highest_seen) > 0)
        {
            break;
        }
        const rx_slot& s = ring[seq & (k_rx_ring - 1)];
        if (s.seq == seq && s.received)
        {
            consider(s.at);
            break;
        }
    }
    if (const auto it = rx_blocks.find(base); it != rx_blocks.end())
    {
        for (const auto& [row, t] : it->second.arrivals)
        {
            (void)row;
            consider(t);
        }
    }
    if (found && nullptr != at)
    {
        *at = best;
    }
    return found;
}

/* The block's first data shard's arrival, else its earliest data shard received. A later shard
 * was pushed after the first, so the estimate errs toward a later deadline. */
bool vstreamer::rs_block_erasure::first_time(uint16_t base, int k_total,
                                             std::chrono::steady_clock::time_point* at) const
{
    bool found = false;
    for (int j = 0; j < k_total; j++)
    {
        const uint16_t seq = static_cast<uint16_t>(base + j);
        const rx_slot& s = ring[seq & (k_rx_ring - 1)];
        if (s.seq == seq && s.received && (!found || s.at < *at))
        {
            *at = s.at;
            found = true;
        }
    }
    return found;
}

bool vstreamer::rs_block_erasure::hole_deadline(uint16_t base, int k_total, int n,
                                                std::chrono::steady_clock::time_point* at) const
{
    clock_type::time_point close {};
    if (!close_time(base, k_total, &close))
    {
        return false;
    }
    const double cap = k_spread_cap_ms / 1000.0;
    double       after = cap;
    if (have_spread_estimate())
    {
        /* Not knowing when the block started means a burst: the longest wait. */
        clock_type::time_point first = close;
        (void)first_time(base, k_total, &first);
        const double fill = std::max(0., seconds(close - first));
        after = std::clamp(last_parity_after_close(fill, k_total, n, spread_est_s) + bias_est_s,
                           0., cap);
    }
    *at = close + seconds_ns(after) + margin_now();
    return true;
}

/*
 * Theil-Sen over the block's parity arrivals against row gives the row gap (the sender's spread
 * over the block's shards) and where the last row lands. The difference from where the sender's
 * release rule puts it, given the block's own close and fill time, is the bias; the scatter of
 * both is the jitter. Blocks are weighted by the parity they gave.
 */
void vstreamer::rs_block_erasure::learn_block(uint16_t base, const rx_block& block)
{
    const auto& a = block.arrivals;
    if (a.empty() || block.n < 1)
    {
        return;
    }
    /* Only a block whose first and last data shards were received has an exact fill and close. */
    const uint16_t last = static_cast<uint16_t>(base + block.k_total - 1);
    const rx_slot& ls = ring[last & (k_rx_ring - 1)];
    const rx_slot& fs = ring[base & (k_rx_ring - 1)];
    if (!(ls.seq == last && ls.received && fs.seq == base && fs.received))
    {
        return;
    }
    const auto   close = ls.at;
    const double fill = std::max(0., seconds(close - fs.at));
    const double c = static_cast<double>(block.k_total + block.n);
    const auto   sec = [close](clock_type::time_point t) { return seconds(t - close); };
    double       gap = -1.;
    double       resid = 0.;
    double       last_s = 0.;
    if (a.size() >= 2)
    {
        std::vector<double> slopes;
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
        if (!slopes.empty())
        {
            gap = std::max(0., median(&slopes));
            std::vector<double> icepts;
            for (const auto& [row, at] : a)
            {
                icepts.push_back(sec(at) - gap * row);
            }
            const double icept = median(&icepts);
            for (const auto& [row, at] : a)
            {
                resid = std::max(resid, std::fabs(sec(at) - (icept + gap * row)));
            }
            last_s = icept + gap * (block.n - 1);
        }
    }
    const double cap = k_spread_cap_ms / 1000.0;
    if (gap < 0.)
    {
        /* One row: place the last one with the spread learnt so far (none: same as this one). */
        const double g = spread_blocks > 0 ? spread_est_s / c : 0.;
        last_s = sec(a.front().second) + g * (block.n - 1 - a.front().first);
    }
    const double spread = gap >= 0. ? std::min(gap * c, cap) : spread_est_s;
    const double err = std::clamp(last_s - last_parity_after_close(fill, block.k_total, block.n,
                                                                   spread),
                                  -cap, cap);
    const double weight = static_cast<double>(a.size()) / static_cast<double>(block.n);
    /* The second sample counts as much as the first; later ones move the estimate by up to half
     * their difference, scaled by how much of the block's parity they saw. */
    const auto alpha = [weight](int samples)
    { return 1 == samples ? 0.5 * weight + 0.25 : 0.5 * weight; };
    if (gap >= 0.)
    {
        spread_est_s = 0 == spread_blocks ? spread
                                          : spread_est_s + alpha(spread_blocks) * (spread -
                                                                                   spread_est_s);
        spread_blocks++;
    }
    if (0 == fitted_blocks)
    {
        bias_est_s = err;
        jitter_est_s = resid;
    }
    else
    {
        const double scatter = std::max(resid, std::fabs(err - bias_est_s));
        bias_est_s += alpha(fitted_blocks) * (err - bias_est_s);
        jitter_est_s += alpha(fitted_blocks) * (scatter - jitter_est_s);
    }
    fitted_blocks++;
}

/* --- blocks --- */

bool vstreamer::rs_block_erasure::block_of(uint16_t seq, uint16_t* base, int* k_total,
                                           int* n) const
{
    for (const auto& [b, block] : rx_blocks)
    {
        const int d = seq_dist(seq, b);
        if (d >= 0 && d < block.k_total)
        {
            *base = b;
            *k_total = block.k_total;
            *n = block.n;
            return true;
        }
    }
    if (!layout_known || layout_k < 1 || layout_step_k < 1)
    {
        return false;
    }
    /* Past the newest block seen, blocks follow it at the stream's steady k; a short block
     * (flushed on a reconfigure) does not set the step. */
    const uint16_t next = static_cast<uint16_t>(layout_base + layout_k);
    const int      step = layout_step_k;
    if (const int d = seq_dist(seq, next); d >= 0)
    {
        *base = static_cast<uint16_t>(next + (d / step) * step);
        *k_total = step;
    }
    else if (seq_dist(seq, layout_base) >= 0)
    {
        *base = layout_base;
        *k_total = layout_k;
    }
    else
    {
        const int back = -seq_dist(seq, layout_base);
        *base = static_cast<uint16_t>(layout_base - ((back + step - 1) / step) * step);
        *k_total = step;
    }
    *n = layout_n;
    return true;
}

/* Counts the parity it never got and learns from it. The block stays as a finished marker while
 * late parity for it can still arrive, so that parity is dropped instead of opening it again. */
void vstreamer::rs_block_erasure::finish_block(uint16_t base, rx_block* block)
{
    if (block->finished)
    {
        return;
    }
    missing_parity_count += static_cast<uint64_t>(block->n - block->parity_have);
    learn_block(base, *block);
    block->finished = true;
    block->done = true;
    block->parity.clear();
    block->arrivals.clear();
}

/* A block is done once all its data is behind the release point and its parity has all come
 * (or stopped coming). It is forgotten once parity for it would be too late to look at. */
void vstreamer::rs_block_erasure::prune_blocks()
{
    const auto t = now();
    for (auto it = rx_blocks.begin(); it != rx_blocks.end();)
    {
        rx_block&  block = it->second;
        const int  end = seq_dist(static_cast<uint16_t>(it->first + block.k_total), emit_next);
        const bool stale = end < -k_max_lo_seq_deviation;
        const bool quiet = t - block.last_seen > std::chrono::milliseconds(k_spread_cap_ms);
        if (!block.finished && end <= 0 && (block.parity_have == block.n || quiet || stale))
        {
            finish_block(it->first, &block);
        }
        if (block.finished && stale)
        {
            it = rx_blocks.erase(it);
            continue;
        }
        ++it;
    }
}

void vstreamer::rs_block_erasure::try_decode(uint16_t base, rx_block* block)
{
    if (block->done)
    {
        return;
    }
    std::vector<bfc::shared_sized_buffer> data(static_cast<size_t>(block->k_total));
    int                                   present = 0;
    bool                                  needed = false;
    for (int j = 0; j < block->k_total; j++)
    {
        const uint16_t seq = static_cast<uint16_t>(base + j);
        const rx_slot* s = slot_for(seq);
        if (s->seq == seq && !s->payload.empty())
        {
            data[static_cast<size_t>(j)] = s->payload;
            present++;
        }
        else if (seq_dist(seq, emit_next) >= 0)
        {
            needed = true;
        }
    }
    if (present == block->k_total)
    {
        block->done = true;
        return;
    }
    if (!needed || present + block->parity_have < block->k_total)
    {
        return;
    }
    int rec = 0;
    if (!decode_rows(block->k_total, block->n, &data, block->parity, &rec))
    {
        rs_failures_count++;
        block->done = true;
        return;
    }
    block->done = true;
    blocks_count++;
    const auto t = now();
    for (int j = 0; j < block->k_total; j++)
    {
        const uint16_t seq = static_cast<uint16_t>(base + j);
        rx_slot*       s = slot_for(seq);
        if ((s->seq == seq && !s->payload.empty()) || seq_dist(seq, emit_next) < 0)
        {
            continue;
        }
        s->payload = std::move(data[static_cast<size_t>(j)]);
        s->seq = seq;
        s->received = false;
        s->at = t;
        recovered_count++;
    }
}

/* --- sequencer --- */

void vstreamer::rs_block_erasure::new_reference(uint16_t seq)
{
    /* The old session's blocks are over: count their missing parity while the ring still has
     * their data. */
    for (auto& [base, block] : rx_blocks)
    {
        finish_block(base, &block);
    }
    for (rx_slot& s : ring)
    {
        s = rx_slot {};
    }
    rx_blocks.clear();
    emit_next = seq;
    highest_seen = seq;
    hole_timed = false;
    gave_up_any = false;
    /* A new session has its own block layout. */
    layout_known = false;
    data_since_parity = 0;
    parity_seen = false;
    new_references_count++;
}

/* Releases everything that can go out in sequence and gives up holes past their deadline. */
void vstreamer::rs_block_erasure::run_emit_engine(fec_rx_payload_list* out)
{
    if (!emit_base_set)
    {
        return;
    }
    const auto t = now();
    while (seq_dist(highest_seen, emit_next) >= 0)
    {
        rx_slot* s = slot_for(emit_next);
        if (s->seq == emit_next && !s->payload.empty())
        {
            /* The ring keeps its own handle: the block may still need it to decode a later
             * hole. */
            if (nullptr != out)
            {
                out->push_back(s->payload);
            }
            emit_next++;
            hole_timed = false;
            continue;
        }
        /* A run of holes shares one wait: it starts at the first hole and ends at the next
         * shard released. */
        if (!hole_timed)
        {
            hole_timed = true;
            hole_since = t;
        }
        uint16_t base = emit_next;
        int      k_total = 0;
        int      n = 0;
        const bool have_block = block_of(emit_next, &base, &k_total, &n);
        if (data_since_parity <= (parity_seen ? k_data_only_after : k_data_only_before_parity))
        {
            clock_type::time_point deadline = hole_since + std::chrono::milliseconds(k_spread_cap_ms);
            if (have_block)
            {
                (void)hole_deadline(base, k_total, n, &deadline);
            }
            if (t < deadline)
            {
                break;
            }
        }
        /* Given up: the sender is past it and its parity is overdue (or there is none). A run of
         * holes with no known block counts as one block. */
        missing_data_count++;
        const bool same_block =
            gave_up_any && (have_block ? gave_up_known && gave_up_base == base
                                       : !gave_up_known &&
                                             static_cast<uint16_t>(gave_up_last + 1) == emit_next);
        if (!same_block)
        {
            given_up_blocks_count++;
        }
        gave_up_any = true;
        gave_up_known = have_block;
        gave_up_base = base;
        gave_up_last = emit_next;
        emit_next++;
    }
}

void vstreamer::rs_block_erasure::push_data(const shard_header& h,
                                            bfc::shared_sized_buffer&& shard,
                                            fec_rx_payload_list* out)
{
    (void)out;
    const uint16_t seq = h.sequence_number;
    if (!emit_base_set)
    {
        emit_base_set = true;
        emit_next = seq;
        highest_seen = static_cast<uint16_t>(seq - 1);
    }
    if (data_since_parity <= k_data_only_after)
    {
        data_since_parity++;
    }
    const int d = seq_dist(seq, emit_next);
    if (d < -k_max_lo_seq_deviation || d > k_max_hi_seq_deviation)
    {
        new_reference(seq);
        highest_seen = static_cast<uint16_t>(seq - 1);
    }
    rx_slot* s = slot_for(seq);
    if (s->seq == seq && !s->payload.empty())
    {
        return; /* duplicate (or already recovered) */
    }
    if (seq_dist(seq, emit_next) < 0)
    {
        late_count++;
    }
    s->payload = shard.subview(h.header_len, shard.size() - h.header_len);
    s->seq = seq;
    s->received = true;
    s->at = now();
    if (seq_dist(seq, highest_seen) > 0)
    {
        highest_seen = seq;
    }
    uint16_t base = 0;
    int      k_total = 0;
    int      n = 0;
    if (block_of(seq, &base, &k_total, &n))
    {
        if (const auto it = rx_blocks.find(base); it != rx_blocks.end())
        {
            try_decode(base, &it->second);
        }
    }
}

void vstreamer::rs_block_erasure::push_parity(const shard_header& h,
                                              bfc::shared_sized_buffer&& shard,
                                              fec_rx_payload_list* out)
{
    (void)out;
    const uint16_t base = h.sequence_number;
    data_since_parity = 0;
    parity_seen = true;
    if (!emit_base_set)
    {
        emit_base_set = true;
        emit_next = base;
        highest_seen = static_cast<uint16_t>(base - 1);
    }
    const int d = seq_dist(base, emit_next);
    if (d > k_max_hi_seq_deviation)
    {
        new_reference(base);
        highest_seen = static_cast<uint16_t>(base - 1);
    }
    else if (d + h.k_total < -k_max_lo_seq_deviation)
    {
        /* Spread parity trails its block, so parity of a block far behind is late, not a new
         * session: a new session behind the release point is found by its data. */
        return;
    }
    /* The block's data all exists at the sender, so a lost last data shard is a hole too. */
    if (const uint16_t last = static_cast<uint16_t>(base + h.k_total - 1);
        seq_dist(last, highest_seen) > 0 && seq_dist(last, emit_next) <= k_max_hi_seq_deviation)
    {
        highest_seen = last;
    }
    if (!layout_known || seq_dist(base, layout_base) > 0)
    {
        const int prev_k = layout_known ? layout_k : h.k_total;
        layout_known = true;
        layout_base = base;
        layout_k = h.k_total;
        layout_n = h.n;
        layout_step_k = std::max(layout_k, prev_k);
    }
    const auto t = now();
    auto       it = rx_blocks.find(base);
    if (it != rx_blocks.end() && it->second.finished)
    {
        return; /* duplicate or late parity of a block already counted */
    }
    if (it != rx_blocks.end() && (it->second.k_total != h.k_total || it->second.n != h.n))
    {
        kn_mismatch_count++;
        finish_block(it->first, &it->second);
        rx_blocks.erase(it);
        it = rx_blocks.end();
    }
    if (it == rx_blocks.end())
    {
        if (rx_blocks.size() >= k_block_max)
        {
            /* Memory bound: drop the block furthest behind; one still open is evicted. */
            auto old = rx_blocks.begin();
            for (auto j = rx_blocks.begin(); j != rx_blocks.end(); ++j)
            {
                if (seq_dist(j->first, emit_next) < seq_dist(old->first, emit_next))
                {
                    old = j;
                }
            }
            if (!old->second.finished)
            {
                evicted_blocks_count++;
                finish_block(old->first, &old->second);
            }
            rx_blocks.erase(old);
        }
        rx_block nb;
        nb.k_total = h.k_total;
        nb.n = h.n;
        nb.parity.resize(static_cast<size_t>(h.n));
        it = rx_blocks.emplace(base, std::move(nb)).first;
    }
    rx_block& block = it->second;
    if (!block.parity[static_cast<size_t>(h.k_i)].empty())
    {
        return;
    }
    block.parity[static_cast<size_t>(h.k_i)] =
        shard.subview(h.header_len, shard.size() - h.header_len);
    block.parity_have++;
    block.arrivals.emplace_back(h.k_i, t);
    block.last_seen = t;
    try_decode(base, &block);
}

void vstreamer::rs_block_erasure::poll_rx(fec_rx_payload_list* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    prune_blocks();
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
    if (shard.empty())
    {
        return;
    }
    shard_header h;
    if (!parse_header(shard.u8(), shard.size(), &h))
    {
        hdr_errors_count++;
        return;
    }
    if (h.is_data)
    {
        push_data(h, std::move(shard), out);
    }
    else
    {
        push_parity(h, std::move(shard), out);
    }
    prune_blocks();
    run_emit_engine(out);
}
