#include "core/rs_block_erasure.hpp"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <mutex>
#include <optional>
#include <utility>
#include <cstring>
#include <random>

extern "C"
{
#include "erasure_code.h"
}

#ifdef VSTREAMER_ISAL_NEON
extern "C" void vstreamer_ec_encode_data(int len, int k, int rows,
                                         unsigned char* g_tbls,
                                         unsigned char** data,
                                         unsigned char** coding);
#define VSTREAMER_EC_ENCODE vstreamer_ec_encode_data
#else
#define VSTREAMER_EC_ENCODE ec_encode_data_base
#endif

namespace
{
constexpr size_t k_min_shard = 16;  // ISA-L NEON kernels need >= 16 bytes.
constexpr int    k_kn_cache = 32;

void store_be16(uint8_t* p, uint16_t v)
{
    const uint16_t n = htons(v);
    memcpy(p, &n, sizeof(n));
}

uint16_t load_be16(const uint8_t* p)
{
    uint16_t n;
    memcpy(&n, p, sizeof(n));
    return ntohs(n);
}

size_t align_shard(size_t row)
{
    return row < k_min_shard ? k_min_shard : row;
}

struct kn_encode_tables
{
    std::vector<uint8_t> encode_matrix;
    std::vector<uint8_t> g_tbls;
};

const kn_encode_tables& kn_encode_tables_for(int k, int n)
{
    static std::mutex mu;
    static std::array<std::array<std::optional<kn_encode_tables>, k_kn_cache>, k_kn_cache> cache;
    std::lock_guard<std::mutex>                          lock(mu);
    std::optional<kn_encode_tables>&                     slot = cache[static_cast<size_t>(k)]
        [static_cast<size_t>(n)];
    if (!slot.has_value())
    {
        kn_encode_tables built;
        const int        p = n - k;
        built.encode_matrix.assign(static_cast<size_t>(n) * static_cast<size_t>(k), 0);
        built.g_tbls.assign(static_cast<size_t>(k) * static_cast<size_t>(p) * 32, 0);
        gf_gen_cauchy1_matrix(built.encode_matrix.data(), n, k);
        ec_init_tables_base(k, p, built.encode_matrix.data() + k * k, built.g_tbls.data());
        slot = std::move(built);
    }
    return *slot;
}
}  // namespace

size_t vstreamer::rs_block_erasure::max_original() const
{
    if (max_shard_bytes <= k_header_len + k_len_prefix)
    {
        return 0;
    }
    return max_shard_bytes - k_header_len - k_len_prefix;
}

const char* vstreamer::rs_block_erasure::impl_name() const
{
#ifdef VSTREAMER_ISAL_NEON
    return "isa-l neon";
#else
    return "isa-l base";
#endif
}

vstreamer::rs_block_erasure::rs_block_erasure()
{
    // Random TX start id: a restarted sender must not replay ids that the
    // receiver's duplicate filter / in-order emit queue still remembers.
    std::random_device rd;
    sdu_seq = static_cast<uint16_t>(rd());
}

bool vstreamer::rs_block_erasure::init(int k, int n, int timeout_ms, size_t max_shard_bytes_in)
{
    if (k < k_header_k_n_min || n < k || n > k_header_k_n_max ||
        k > k_header_k_n_max || timeout_ms < 0 ||
        max_shard_bytes_in < k_header_len + k_len_prefix + 1)
    {
        return false;
    }
    active = false;
    cfg_k = k;
    cfg_n = n;
    p = n - k;
    this->timeout_ms = timeout_ms;
    max_shard_bytes = max_shard_bytes_in;
    const kn_encode_tables& tables = kn_encode_tables_for(cfg_k, cfg_n);
    encode_matrix = tables.encode_matrix;
    g_tbls = tables.g_tbls;
    pending.clear();
    deadline_set = false;
    rx_blocks.clear();
    done_order.clear();
    done.clear();
    ready_blocks.clear();
    emit_base_set = false;
    emit_next = 0;
    newest_set = false;
    have_payload_emit = false;
    have_shard_rx = false;
    far_behind_held.clear();
    later_block_waiting = false;
    recovered_count = 0;
    recovered_seen = 0;
    blocks_count = 0;
    decode_fail_seen = 0;
    hdr_errors_count = 0;
    hdr_errors_seen = 0;
    kn_mismatch_count = 0;
    kn_mismatch_seen = 0;
    evicted_blocks_count = 0;
    evicted_blocks_seen = 0;
    rs_failures_count = 0;
    rs_failures_seen = 0;
    missing_shards_count = 0;
    missing_shards_seen = 0;
    fail_lost_app_pkts_count = 0;
    fail_lost_app_pkts_seen = 0;
    late_blocks_count = 0;
    late_blocks_seen = 0;
    oversized_count = 0;
    active = true;
    return true;
}

void vstreamer::rs_block_erasure::disable()
{
    active = false;
    pending.clear();
    deadline_set = false;
    cfg_k = 0;
    cfg_n = 0;
    p = 0;
    encode_matrix.clear();
    g_tbls.clear();
    rx_blocks.clear();
    done_order.clear();
    done.clear();
    ready_blocks.clear();
    emit_base_set = false;
    emit_next = 0;
    newest_set = false;
    have_payload_emit = false;
    have_shard_rx = false;
    far_behind_held.clear();
    later_block_waiting = false;
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

void vstreamer::rs_block_erasure::record_payload_emit()
{
    last_payload_emit = now();
    have_payload_emit = true;
}

void vstreamer::rs_block_erasure::emit_payload(fec_rx_payload_list* out,
                                               shared_sized_buffer&& app)
{
    if (nullptr == out || app.empty())
    {
        return;
    }
    out->push_back(std::move(app));
    record_payload_emit();
}

bool vstreamer::rs_block_erasure::frag_to_app(const shared_sized_buffer& shard,
                                              shared_sized_buffer* app)
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

void vstreamer::rs_block_erasure::note_emit_base(uint16_t sdu_base)
{
    if (!emit_base_set)
    {
        emit_base_set = true;
        emit_next = sdu_base;
        newest = emit_next;
        newest_set = true;
    }
}

void vstreamer::rs_block_erasure::touch_newest(uint16_t sdu_base)
{
    if (!newest_set)
    {
        newest = sdu_base;
        newest_set = true;
        return;
    }
    if (ring_dist(sdu_base, newest) > 0)
    {
        newest = sdu_base;
    }
}

void vstreamer::rs_block_erasure::clear_state_behind(uint16_t base)
{
    for (auto it = rx_blocks.begin(); it != rx_blocks.end();)
    {
        if (ring_dist(it->first, base) < 0)
        {
            it = rx_blocks.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = ready_blocks.begin(); it != ready_blocks.end();)
    {
        if (ring_dist(it->first, base) < 0)
        {
            it = ready_blocks.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = done.begin(); it != done.end();)
    {
        if (ring_dist(it->first, base) < 0)
        {
            done_order.erase(std::remove(done_order.begin(), done_order.end(), it->first),
                             done_order.end());
            it = done.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

/* Keeps only blocks in [base, base + k_ring_evict_sdus]: on a sender session change those are the
 * new session's, everything else (including the done set) belongs to the old one. */
void vstreamer::rs_block_erasure::keep_state_ahead_of(uint16_t base)
{
    const auto outside = [base](uint16_t id)
    {
        const int d = ring_dist(id, base);
        return d < 0 || d > k_ring_evict_sdus;
    };
    for (auto it = rx_blocks.begin(); it != rx_blocks.end();)
    {
        if (outside(it->first))
        {
            evicted_blocks_count++;
            it = rx_blocks.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = ready_blocks.begin(); it != ready_blocks.end();)
    {
        if (outside(it->first))
        {
            it = ready_blocks.erase(it);
        }
        else
        {
            ++it;
        }
    }
    done.clear();
    done_order.clear();
    emit_next = base;
    newest = base;
    newest_set = true;
    for (const auto& kv : rx_blocks)
    {
        touch_newest(kv.first);
    }
    for (const auto& kv : ready_blocks)
    {
        touch_newest(kv.first);
    }
    later_block_waiting = false;
}

/* A shard further behind emit_next than any real late shard is either a stray of the old session
 * (reordered past a resync) or a new sender session whose random sdu_base landed behind. Hold it
 * until maybe_resync_to_held() can tell the two apart. */
bool vstreamer::rs_block_erasure::hold_far_behind_shard(uint16_t sdu_base,
                                                        const shared_sized_buffer& shard,
                                                        fec_rx_payload_list* out)
{
    const auto t = now();
    if (dist_from_emit(sdu_base) >= -k_ring_evict_sdus)
    {
        last_in_window_rx = t;
        const auto stale = std::chrono::milliseconds(emit_hold_ms());
        while (!far_behind_held.empty() && t - far_behind_held.front().at > stale)
        {
            far_behind_held.pop_front();
        }
        return false;
    }
    if (far_behind_held.size() >= k_far_behind_hold_max)
    {
        far_behind_held.pop_front();
    }
    far_behind_held.push_back({t, shard});
    maybe_resync_to_held(out);
    return true;
}

/* Once no in-window shard has arrived for emit_hold_ms the old session is gone: resync to the
 * earliest held base of the newest held session and replay its shards in arrival order. */
void vstreamer::rs_block_erasure::maybe_resync_to_held(fec_rx_payload_list* out)
{
    if (far_behind_held.empty())
    {
        return;
    }
    const auto t = now();
    if (t - last_in_window_rx < std::chrono::milliseconds(emit_hold_ms()))
    {
        return;
    }
    const uint16_t ref = load_be16(far_behind_held.back().shard.u8());
    const auto in_session = [ref](uint16_t id)
    {
        const int d = ring_dist(id, ref);
        return d <= 0 && d >= -k_ring_evict_sdus;
    };
    uint16_t start = ref;
    for (const auto& h : far_behind_held)
    {
        const uint16_t id = load_be16(h.shard.u8());
        if (in_session(id) && ring_dist(id, start) < 0)
        {
            start = id;
        }
    }
    std::deque<held_shard_s> replay;
    replay.swap(far_behind_held);
    evicted_blocks_count += rx_blocks.size();
    rx_blocks.clear();
    ready_blocks.clear();
    keep_state_ahead_of(start);
    last_in_window_rx = t;
    for (auto& h : replay)
    {
        if (!in_session(load_be16(h.shard.u8())))
        {
            continue;
        }
        fec_rx_payload_list step;
        push_air(std::move(h.shard), &step);
        if (nullptr != out)
        {
            out->insert(out->end(), std::make_move_iterator(step.begin()),
                        std::make_move_iterator(step.end()));
        }
    }
}

void vstreamer::rs_block_erasure::maybe_resync_on_late_shard(uint16_t sdu_base)
{
    if (dist_from_emit(sdu_base) >= 0)
    {
        return;
    }
    if (!have_payload_emit)
    {
        return;
    }
    const auto t = now();
    if (t - last_payload_emit < std::chrono::milliseconds(rx_hold_ms()))
    {
        return;
    }
    emit_next = sdu_base;
    newest = sdu_base;
    newest_set = true;
    clear_state_behind(sdu_base);
    later_block_waiting = false;
}

/* Resync, either direction: after rx_hold_ms with no shard at all, the next shard starts a
 * new session (peer restart or link back up). Its block id is random relative to emit_next, so
 * holding it behind never-coming gap ids (forward jump) would stall delivery and, once the ids
 * cross the half ring, let the backward rebase discard it. Everything held is from the old
 * session (expire_rx has already given up on it), so drop it and emit from this block. */
void vstreamer::rs_block_erasure::maybe_rebase_after_silence(uint16_t sdu_base)
{
    const auto t = now();
    const bool rebase = have_shard_rx && emit_base_set &&
                        t - last_shard_rx >= std::chrono::milliseconds(rx_hold_ms());
    last_shard_rx = t;
    have_shard_rx = true;
    if (!rebase || dist_from_emit(sdu_base) == 0)
    {
        return;
    }
    evicted_blocks_count += rx_blocks.size();
    rx_blocks.clear();
    ready_blocks.clear();
    done.clear();
    done_order.clear();
    far_behind_held.clear();
    emit_next = sdu_base;
    newest = emit_next;
    newest_set = true;
    later_block_waiting = false;
}

void vstreamer::rs_block_erasure::ring_evict_stale(fec_rx_payload_list* out)
{
    if (!newest_set)
    {
        return;
    }
    for (auto it = rx_blocks.begin(); it != rx_blocks.end();)
    {
        const uint16_t bid = it->first;
        if (ring_dist(bid, newest) < -k_ring_evict_sdus)
        {
            evicted_blocks_count++;
            const rx_block_s snap = it->second;
            const uint16_t  eid = it->first;
            it = rx_blocks.erase(it);
            abandon_partial_block(snap, eid, out);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = done.begin(); it != done.end();)
    {
        if (ring_dist(it->first, newest) < -k_ring_evict_sdus)
        {
            done_order.erase(std::remove(done_order.begin(), done_order.end(), it->first),
                             done_order.end());
            it = done.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void vstreamer::rs_block_erasure::try_stream_head_systematic(
    rx_block_s& block, fec_rx_payload_list* out)
{
    const int sn = expected_sdus(block);
    while (block.released < sn)
    {
        const auto it = block.frags.find(block.released);
        if (it == block.frags.end())
        {
            break;
        }
        shared_sized_buffer app;
        if (!frag_to_app(it->second, &app))
        {
            break;
        }
        emit_payload(out, std::move(app));
        block.released++;
    }
}

void vstreamer::rs_block_erasure::abandon_partial_block(
    const rx_block_s& block, uint16_t sdu_base, fec_rx_payload_list* out)
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
        shared_sized_buffer app;
        if (!frag_to_app(it->second, &app))
        {
            continue;
        }
        emit_payload(out, std::move(app));
        emitted++;
    }
    note_rx_block_output_shortfall(sn, block.released + emitted);
    if (sdu_base == emit_next)
    {
        mark_done(emit_next);
        emit_next = static_cast<uint16_t>(emit_next + static_cast<uint16_t>(sn));
        later_block_waiting = false;
    }
}

void vstreamer::rs_block_erasure::on_block_decoded(uint16_t sdu_base, int released_before,
                                                   fec_rx_payload_list payloads, int sdu_n,
                                                   fec_rx_payload_list* out)
{
    const int d = dist_from_emit(sdu_base);
    if (d < 0)
    {
        late_blocks_count++;
        for (int i = released_before; i < sdu_n && i < static_cast<int>(payloads.size()); i++)
        {
            emit_payload(out, std::move(payloads[static_cast<size_t>(i)]));
        }
        mark_done(sdu_base);
        return;
    }
    if (sdu_base == emit_next)
    {
        for (int i = released_before; i < sdu_n && i < static_cast<int>(payloads.size()); i++)
        {
            emit_payload(out, std::move(payloads[static_cast<size_t>(i)]));
        }
        mark_done(emit_next);
        emit_next = static_cast<uint16_t>(emit_next + static_cast<uint16_t>(sdu_n));
        later_block_waiting = false;
        run_emit_engine(out);
        return;
    }
    ready_block_s ready;
    ready.payloads = std::move(payloads);
    ready.released = released_before;
    ready.sdu_n = sdu_n;
    ready_blocks[sdu_base] = std::move(ready);
    if (d > 0 && !later_block_waiting)
    {
        later_block_waiting = true;
        later_block_since = now();
    }
}

void vstreamer::rs_block_erasure::maybe_give_up_head(fec_rx_payload_list* out)
{
    if (!emit_base_set)
    {
        return;
    }
    const auto t = now();
    bool       give_up = false;
    auto       rxit = rx_blocks.find(emit_next);
    if (rxit != rx_blocks.end())
    {
        const rx_block_s& block = rxit->second;
        if (static_cast<int>(block.frags.size()) >= block.n)
        {
            give_up = true;
        }
        else if (later_block_waiting &&
                 t - later_block_since > std::chrono::milliseconds(emit_hold_ms()))
        {
            give_up = true;
        }
        if (give_up)
        {
            const rx_block_s snap = block;
            rx_blocks.erase(rxit);
            abandon_partial_block(snap, emit_next, out);
            run_emit_engine(out);
        }
        return;
    }
    if (ready_blocks.find(emit_next) != ready_blocks.end())
    {
        return;
    }
    if (later_block_waiting &&
        t - later_block_since > std::chrono::milliseconds(emit_hold_ms()))
    {
        bool     found = false;
        uint16_t target = emit_next;
        int      best_d = 0;
        for (const auto& kv : ready_blocks)
        {
            const int d = dist_from_emit(kv.first);
            if (d > 0 && (!found || d < best_d))
            {
                found = true;
                target = kv.first;
                best_d = d;
            }
        }
        for (const auto& kv : rx_blocks)
        {
            const int d = dist_from_emit(kv.first);
            if (d > 0 && (!found || d < best_d))
            {
                found = true;
                target = kv.first;
                best_d = d;
            }
        }
        if (found)
        {
            if (best_d > static_cast<int>(k_ring_evict_sdus))
            {
                /* Larger than any real burst: a new sender session, not loss. */
                keep_state_ahead_of(target);
            }
            else
            {
                note_head_hole_sdus(best_d);
                emit_next = target;
            }
        }
        later_block_waiting = false;
        run_emit_engine(out);
    }
}

void vstreamer::rs_block_erasure::run_emit_engine(fec_rx_payload_list* out)
{
    if (!emit_base_set)
    {
        return;
    }
    for (;;)
    {
        bool progressed = false;
        auto rit = ready_blocks.find(emit_next);
        if (rit != ready_blocks.end())
        {
            ready_block_s& rb = rit->second;
            for (int i = rb.released; i < rb.sdu_n && i < static_cast<int>(rb.payloads.size());
                 i++)
            {
                emit_payload(out, std::move(rb.payloads[static_cast<size_t>(i)]));
            }
            const int released_sdu_n = rb.sdu_n;
            ready_blocks.erase(rit);
            mark_done(emit_next);
            emit_next = static_cast<uint16_t>(emit_next + static_cast<uint16_t>(released_sdu_n));
            progressed = true;
            continue;
        }
        auto rxit = rx_blocks.find(emit_next);
        if (rxit != rx_blocks.end())
        {
            const int before = rxit->second.released;
            try_stream_head_systematic(rxit->second, out);
            if (rxit->second.released != before)
            {
                progressed = true;
            }
        }
        if (!progressed)
        {
            break;
        }
    }
    maybe_give_up_head(out);
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
    int                  rec = 0;
    fec_rx_payload_list  decoded;
    const rx_block_s     snap = *block;
    const int            released_before = block->released;
    const bool           ok =
        decode_block(block->k, block->n, block->sdu_n, block->frags, &decoded, &rec);
    rx_blocks.erase(sdu_base);
    if (!ok)
    {
        rs_failures_count++;
        abandon_partial_block(snap, sdu_base, out);
        run_emit_engine(out);
        return true;
    }
    recovered_count += static_cast<uint64_t>(rec);
    blocks_count++;
    note_rx_block_output_shortfall(expected_sdus(snap), static_cast<int>(decoded.size()));
    on_block_decoded(sdu_base, released_before, std::move(decoded), snap.sdu_n, out);
    return true;
}

bool vstreamer::rs_block_erasure::pack_header(uint8_t* out, uint16_t sdu_base, int index, int k,
                                              int n, int sdu_n)
{
    if (out == nullptr || index < 0 || index >= n || k < k_header_k_n_min ||
        k > k_header_k_n_max || n < k || n > k_header_k_n_max || sdu_n < 1 || sdu_n > k)
    {
        return false;
    }
    store_be16(out, sdu_base);
    const uint16_t fec_config = static_cast<uint16_t>(
        ((static_cast<uint16_t>(k) & 0x1FU) << 11) |
        ((static_cast<uint16_t>(n) & 0x1FU) << 6) |
        ((static_cast<uint16_t>(index) & 0x1FU) << 1));
    store_be16(out + 2, fec_config);
    out[4] = static_cast<uint8_t>((static_cast<uint8_t>(sdu_n) & 0x1FU) << 3);
    return true;
}

bool vstreamer::rs_block_erasure::unpack_header(const uint8_t* data, size_t len,
                                                uint16_t* sdu_base, int* index, int* k, int* n,
                                                int* sdu_n)
{
    if (data == nullptr || len < k_header_len || sdu_base == nullptr || index == nullptr ||
        k == nullptr || n == nullptr || sdu_n == nullptr)
    {
        return false;
    }
    const uint16_t fec_config = load_be16(data + 2);
    if (0 != (fec_config & k_wire_fec_config_spare))
    {
        return false;
    }
    const int kk = static_cast<int>((fec_config >> 11) & 0x1F);
    const int nn = static_cast<int>((fec_config >> 6) & 0x1F);
    const int idx = static_cast<int>((fec_config >> 1) & 0x1F);
    const uint8_t fec2 = data[4];
    if (0 != (fec2 & k_wire_fec2_spare))
    {
        return false;
    }
    const int sn = static_cast<int>((fec2 >> 3) & 0x1F);
    if (kk < k_header_k_n_min || kk > k_header_k_n_max || nn < kk || nn > k_header_k_n_max ||
        idx >= nn || sn < 1 || sn > kk)
    {
        return false;
    }
    *sdu_base = load_be16(data);
    *index = idx;
    *k = kk;
    *n = nn;
    *sdu_n = sn;
    return true;
}

bool vstreamer::rs_block_erasure::encode_block(
    const std::vector<std::vector<uint8_t>>& packets, uint16_t sdu_base,
    std::vector<std::vector<uint8_t>>* out) const
{
    if (!active || out == nullptr || packets.size() > static_cast<size_t>(cfg_k))
    {
        return false;
    }
    std::vector<std::vector<uint8_t>> data_shards(static_cast<size_t>(cfg_k));
    size_t max_row = 0;
    for (int i = 0; i < cfg_k; i++)
    {
        const std::vector<uint8_t>* pkt = i < static_cast<int>(packets.size())
                                              ? &packets[static_cast<size_t>(i)]
                                              : nullptr;
        const size_t plen = pkt != nullptr ? pkt->size() : 0;
        if (plen > max_original())
        {
            return false;
        }
        data_shards[static_cast<size_t>(i)].resize(k_len_prefix + plen);
        store_be16(data_shards[static_cast<size_t>(i)].data(),
                   static_cast<uint16_t>(plen));
        if (plen > 0)
        {
            memcpy(data_shards[static_cast<size_t>(i)].data() + k_len_prefix,
                   pkt->data(), plen);
        }
        max_row = std::max(max_row, data_shards[static_cast<size_t>(i)].size());
    }
    const size_t shard_len = align_shard(max_row);
    if (k_header_len + shard_len > max_shard_bytes)
    {
        return false;
    }
    for (int i = 0; i < cfg_k; i++)
    {
        data_shards[static_cast<size_t>(i)].resize(shard_len, 0);
    }
    std::vector<std::vector<uint8_t>> parity(static_cast<size_t>(p));
    std::vector<unsigned char*> data_ptrs(static_cast<size_t>(cfg_k));
    std::vector<unsigned char*> coding_ptrs(static_cast<size_t>(p));
    for (int i = 0; i < cfg_k; i++)
    {
        data_ptrs[static_cast<size_t>(i)] =
            data_shards[static_cast<size_t>(i)].data();
    }
    for (int i = 0; i < p; i++)
    {
        parity[static_cast<size_t>(i)].assign(shard_len, 0);
        coding_ptrs[static_cast<size_t>(i)] =
            parity[static_cast<size_t>(i)].data();
    }
    if (p > 0)
    {
        VSTREAMER_EC_ENCODE(static_cast<int>(shard_len), cfg_k, p,
                          const_cast<unsigned char*>(g_tbls.data()),
                          data_ptrs.data(), coding_ptrs.data());
    }

    out->clear();
    const int sdu_n = static_cast<int>(packets.size());
    for (int i = 0; i < cfg_n; i++)
    {
        if (i < cfg_k && i >= sdu_n)
        {
            continue;
        }
        size_t body_len = shard_len;
        const uint8_t* body = nullptr;
        if (i < cfg_k)
        {
            const size_t plen = packets[static_cast<size_t>(i)].size();
            body_len = k_len_prefix + plen;
            body = data_shards[static_cast<size_t>(i)].data();
        }
        else
        {
            body = parity[static_cast<size_t>(i - cfg_k)].data();
        }
        std::vector<uint8_t> pkt(k_header_len + body_len);
        pack_header(pkt.data(), sdu_base, i, cfg_k, cfg_n, sdu_n);
        memcpy(pkt.data() + k_header_len, body, body_len);
        out->push_back(std::move(pkt));
    }
    return true;
}

int vstreamer::rs_block_erasure::gen_decode_matrix(int k, int n,
                                        const uint8_t* encode_matrix,
                                        const uint8_t* err_list, int nerrs,
                                        uint8_t* decode_matrix,
                                        uint8_t* decode_index)
{
    if (encode_matrix == nullptr || err_list == nullptr ||
        decode_matrix == nullptr || decode_index == nullptr || k < 1 ||
        n <= k)
    {
        return -1;
    }
    std::vector<uint8_t> in_err(static_cast<size_t>(n), 0);
    for (int i = 0; i < nerrs; i++)
    {
        if (err_list[i] >= n)
        {
            return -1;
        }
        in_err[err_list[i]] = 1;
    }
    std::vector<uint8_t> b(static_cast<size_t>(k) * static_cast<size_t>(k));
    std::vector<uint8_t> invert(static_cast<size_t>(k) *
                                static_cast<size_t>(k));
    int r = 0;
    for (int i = 0; i < k; i++, r++)
    {
        while (r < n && in_err[static_cast<size_t>(r)])
        {
            r++;
        }
        if (r >= n)
        {
            return -1;
        }
        memcpy(b.data() + static_cast<size_t>(k) * static_cast<size_t>(i),
               encode_matrix + static_cast<size_t>(k) * static_cast<size_t>(r),
               static_cast<size_t>(k));
        decode_index[i] = static_cast<uint8_t>(r);
    }
    if (gf_invert_matrix(b.data(), invert.data(), k) < 0)
    {
        return -1;
    }
    for (int e = 0; e < nerrs; e++)
    {
        const int idx = err_list[e];
        if (idx < k)
        {
            memcpy(decode_matrix +
                       static_cast<size_t>(k) * static_cast<size_t>(e),
                   invert.data() +
                       static_cast<size_t>(k) * static_cast<size_t>(idx),
                   static_cast<size_t>(k));
            continue;
        }
        for (int i = 0; i < k; i++)
        {
            uint8_t s = 0;
            for (int j = 0; j < k; j++)
            {
                s ^= gf_mul(
                    invert[static_cast<size_t>(j) * static_cast<size_t>(k) +
                           static_cast<size_t>(i)],
                    encode_matrix[static_cast<size_t>(k) *
                                       static_cast<size_t>(idx) +
                                   static_cast<size_t>(j)]);
            }
            decode_matrix[static_cast<size_t>(k) * static_cast<size_t>(e) +
                          static_cast<size_t>(i)] = s;
        }
    }
    return 0;
}

bool vstreamer::rs_block_erasure::decode_block(
    const std::unordered_map<int, shared_sized_buffer>& frags, fec_rx_payload_list* payloads,
    int* recovered) const
{
    if (!active)
    {
        return false;
    }
    return decode_block(cfg_k, cfg_n, cfg_k, frags, payloads, recovered);
}

bool vstreamer::rs_block_erasure::decode_block(
    int k, int n, int sdu_n, const std::unordered_map<int, shared_sized_buffer>& frags,
    fec_rx_payload_list* payloads, int* recovered) const
{
    if (payloads == nullptr || k < 1 || n < k || n > static_cast<int>(k_max_n) ||
        sdu_n < 0 || sdu_n > k)
    {
        return false;
    }
    size_t shard_slots = frags.size();
    for (int i = sdu_n; i < k; i++)
    {
        if (frags.find(i) == frags.end())
        {
            shard_slots++;
        }
    }
    if (shard_slots < static_cast<size_t>(k))
    {
        return false;
    }
    const int               parity = n - k;
    const kn_encode_tables& tables = kn_encode_tables_for(k, n);
    const uint8_t*          encode_matrix = tables.encode_matrix.data();

    size_t shard_len = k_len_prefix;
    for (const auto& kv : frags)
    {
        if (kv.first < 0 || kv.first >= n || kv.second.size() < k_header_len)
        {
            return false;
        }
        const size_t body_len = kv.second.size() - k_header_len;
        shard_len = std::max(shard_len, body_len);
    }
    shard_len = align_shard(shard_len);

    std::array<uint8_t, k_max_n>              have {};
    std::array<std::vector<uint8_t>, k_max_n> owned {};
    for (const auto& kv : frags)
    {
        const int idx = kv.first;
        have[static_cast<size_t>(idx)] = 1;
        const uint8_t* body = kv.second.u8() + k_header_len;
        const size_t   body_len = kv.second.size() - k_header_len;
        if (body_len < shard_len)
        {
            owned[static_cast<size_t>(idx)].assign(body, body + body_len);
            owned[static_cast<size_t>(idx)].resize(shard_len, 0);
        }
    }
    for (int i = sdu_n; i < k; i++)
    {
        if (frags.find(i) != frags.end())
        {
            continue;
        }
        have[static_cast<size_t>(i)] = 1;
        owned[static_cast<size_t>(i)].assign(shard_len, 0);
    }

    const auto row_ref = [&](int idx) -> const std::vector<uint8_t>& {
        if (!owned[static_cast<size_t>(idx)].empty())
        {
            return owned[static_cast<size_t>(idx)];
        }
        const shared_sized_buffer& shard = frags.at(idx);
        owned[static_cast<size_t>(idx)].assign(shard.u8() + k_header_len,
                                                shard.u8() + shard.size());
        if (owned[static_cast<size_t>(idx)].size() < shard_len)
        {
            owned[static_cast<size_t>(idx)].resize(shard_len, 0);
        }
        return owned[static_cast<size_t>(idx)];
    };

    bool have_all_systematic = true;
    for (int i = 0; i < k; i++)
    {
        if (!have[static_cast<size_t>(i)])
        {
            have_all_systematic = false;
            break;
        }
    }

    std::vector<std::vector<uint8_t>> data_copy(static_cast<size_t>(k));
    int                               rec = 0;
    if (!have_all_systematic)
    {
        for (int i = 0; i < k; i++)
        {
            if (have[static_cast<size_t>(i)])
            {
                data_copy[static_cast<size_t>(i)] = row_ref(i);
            }
        }
        uint8_t err_list[k_max_n];
        int     nerrs = 0;
        for (int i = 0; i < n; i++)
        {
            if (!have[static_cast<size_t>(i)])
            {
                err_list[nerrs++] = static_cast<uint8_t>(i);
            }
        }
        if (nerrs > parity)
        {
            return false;
        }
        std::vector<uint8_t> decode_matrix(static_cast<size_t>(nerrs) *
                                           static_cast<size_t>(k));
        uint8_t decode_index[k_max_n];
        if (gen_decode_matrix(k, n, encode_matrix, err_list, nerrs, decode_matrix.data(),
                              decode_index) != 0)
        {
            return false;
        }
        std::vector<uint8_t>              decode_tbls(static_cast<size_t>(k) *
                                                      static_cast<size_t>(nerrs) * 32);
        std::vector<unsigned char*>       src_ptrs(static_cast<size_t>(k));
        std::vector<std::vector<uint8_t>> recover(static_cast<size_t>(nerrs));
        std::vector<unsigned char*>       rec_ptrs(static_cast<size_t>(nerrs));
        for (int i = 0; i < k; i++)
        {
            const int idx = decode_index[i];
            if (!have[static_cast<size_t>(idx)])
            {
                return false;
            }
            src_ptrs[static_cast<size_t>(i)] =
                const_cast<unsigned char*>(row_ref(idx).data());
        }
        for (int i = 0; i < nerrs; i++)
        {
            recover[static_cast<size_t>(i)].assign(shard_len, 0);
            rec_ptrs[static_cast<size_t>(i)] = recover[static_cast<size_t>(i)].data();
        }
        ec_init_tables_base(k, nerrs, decode_matrix.data(), decode_tbls.data());
        VSTREAMER_EC_ENCODE(static_cast<int>(shard_len), k, nerrs, decode_tbls.data(),
                            src_ptrs.data(), rec_ptrs.data());
        for (int i = 0; i < nerrs; i++)
        {
            const int idx = err_list[i];
            if (idx < k)
            {
                data_copy[static_cast<size_t>(idx)] = std::move(recover[static_cast<size_t>(i)]);
                rec++;
            }
        }
    }

    payloads->clear();
    for (int i = 0; i < k; i++)
    {
        if (have_all_systematic)
        {
            if (!have[static_cast<size_t>(i)])
            {
                continue;
            }
            if (frags.find(i) != frags.end())
            {
                shared_sized_buffer app;
                if (!frag_to_app(frags.at(i), &app))
                {
                    return false;
                }
                if (app.empty())
                {
                    continue;
                }
                payloads->push_back(std::move(app));
                continue;
            }
            const std::vector<uint8_t>& row = owned[static_cast<size_t>(i)];
            if (row.size() < k_len_prefix)
            {
                return false;
            }
            const uint16_t orig_len = load_be16(row.data());
            if (orig_len == 0)
            {
                continue;
            }
            payloads->push_back(
                shared_sized_buffer::copy_from(row.data() + k_len_prefix, orig_len));
            continue;
        }
        const std::vector<uint8_t>& row = data_copy[static_cast<size_t>(i)];
        if (row.empty() || row.size() < k_len_prefix)
        {
            return false;
        }
        const uint16_t orig_len = load_be16(row.data());
        if (orig_len == 0)
        {
            continue;
        }
        if (k_len_prefix + orig_len > row.size())
        {
            return false;
        }
        payloads->push_back(shared_sized_buffer::copy_from(row.data() + k_len_prefix, orig_len));
    }
    if (recovered != nullptr)
    {
        *recovered = rec;
    }
    return true;
}

void vstreamer::rs_block_erasure::push_app(const uint8_t* data, size_t len,
                              std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!active || out == nullptr || data == nullptr)
    {
        return;
    }
    if (len > max_original())
    {
        oversized_count++;
        return;
    }
    pending.emplace_back(data, data + len);
    if (!deadline_set)
    {
        deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
        deadline_set = true;
    }
    if (static_cast<int>(pending.size()) >= cfg_k)
    {
        flush(out);
    }
}

void vstreamer::rs_block_erasure::flush(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!active || out == nullptr || pending.empty())
    {
        return;
    }
    const int sdu_n = static_cast<int>(pending.size());
    if (!encode_block(pending, sdu_seq, out))
    {
        oversized_count++;
        out->clear();
        pending.clear();
        deadline_set = false;
        return;
    }
    sdu_seq = static_cast<uint16_t>(sdu_seq + static_cast<uint16_t>(sdu_n));
    pending.clear();
    deadline_set = false;
    blocks_count++;
}

int vstreamer::rs_block_erasure::rx_hold_ms() const
{
    return std::max(timeout_ms * 10, 250);
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
    if (block.sdu_n > 0)
    {
        return block.sdu_n;
    }
    return block.k;
}


int vstreamer::rs_block_erasure::done_hold_ms() const
{
    return std::max(timeout_ms * 50, rx_hold_ms());
}

int vstreamer::rs_block_erasure::emit_hold_ms() const
{
    // Shards of block N are all sent before block N+1 starts, so a block
    // that completes after a later one is only a few datagrams reordered;
    // a short window suffices and keeps head-of-line delay bounded.
    return std::max(timeout_ms * 3, 60);
}

void vstreamer::rs_block_erasure::poll_rx(fec_rx_payload_list* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    maybe_resync_to_held(out);
    expire_rx(out);
    run_emit_engine(out);
}

std::chrono::steady_clock::time_point vstreamer::rs_block_erasure::now() const
{
    return std::chrono::steady_clock::now();
}

void vstreamer::rs_block_erasure::on_tick(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    fec_rx_payload_list rx_expired;
    expire_rx(&rx_expired);
    if (!active || !deadline_set || timeout_ms <= 0)
    {
        return;
    }
    if (std::chrono::steady_clock::now() < deadline)
    {
        return;
    }
    flush(out);
}

bool vstreamer::rs_block_erasure::next_deadline(
    std::chrono::steady_clock::time_point* out) const
{
    if (!active || !deadline_set || timeout_ms <= 0)
    {
        return false;
    }
    if (nullptr != out)
    {
        *out = deadline;
    }
    return true;
}

void vstreamer::rs_block_erasure::expire_rx(fec_rx_payload_list* out)
{
    expire_done();
    if (rx_blocks.empty())
    {
        return;
    }
    const auto t = now();
    const auto hold = std::chrono::milliseconds(rx_hold_ms());
    for (auto it = rx_blocks.begin(); it != rx_blocks.end();)
    {
        if (t - it->second.last_seen > hold)
        {
            evicted_blocks_count++;
            const uint16_t expired_id = it->first;
            const rx_block_s block_snap = it->second;
            it = rx_blocks.erase(it);
            abandon_partial_block(block_snap, expired_id, out);
        }
        else
        {
            ++it;
        }
    }
}

void vstreamer::rs_block_erasure::expire_done()
{
    const auto t = now();
    const auto hold = std::chrono::milliseconds(done_hold_ms());
    while (!done_order.empty())
    {
        const uint16_t id = done_order.front();
        auto it = done.find(id);
        if (it == done.end())
        {
            done_order.pop_front();
            continue;
        }
        if (t - it->second <= hold)
        {
            break;
        }
        done.erase(it);
        done_order.pop_front();
    }
}

void vstreamer::rs_block_erasure::mark_done(uint16_t sdu_base)
{
    const auto t = now();
    auto inserted = done.emplace(sdu_base, t);
    if (inserted.second)
    {
        done_order.push_back(sdu_base);
    }
    else
    {
        inserted.first->second = t;
    }
    while (done_order.size() > k_done_max)
    {
        done.erase(done_order.front());
        done_order.pop_front();
    }
}

void vstreamer::rs_block_erasure::push_air(const uint8_t* data, size_t len,
                                           fec_rx_payload_list* out)
{
    if (nullptr == data || 0 == len)
    {
        return;
    }
    push_air(shared_sized_buffer::copy_from(data, len), out);
}

void vstreamer::rs_block_erasure::push_air(shared_sized_buffer shard,
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
    const uint8_t* data = shard.u8();
    const size_t   len = shard.size();
    // RX is always FEC-aware: k/n come from the shard header. Encode still
    // requires init().
    if (len < k_header_len)
    {
        hdr_errors_count++;
        return;
    }
    struct push_air_finish
    {
        rs_block_erasure*                     self;
        fec_rx_payload_list* out;
        ~push_air_finish()
        {
            self->expire_rx(out);
            self->run_emit_engine(out);
        }
    } finish {this, out};
    uint16_t sdu_base = 0;
    int index = 0;
    int k = 0;
    int n = 0;
    int sdu_n = 0;
    if (!unpack_header(data, len, &sdu_base, &index, &k, &n, &sdu_n))
    {
        hdr_errors_count++;
        return;
    }
    note_emit_base(sdu_base);
    maybe_rebase_after_silence(sdu_base);
    if (hold_far_behind_shard(sdu_base, shard, out))
    {
        return;
    }
    touch_newest(sdu_base);
    maybe_resync_on_late_shard(sdu_base);
    ring_evict_stale(out);

    const int d = dist_from_emit(sdu_base);
    if (d < 0 && done.find(sdu_base) != done.end())
    {
        return;
    }
    /* Already decoded and waiting for the head: a late (parity) shard is redundant. Creating a
     * new rx block here would later be abandoned and count the whole block as lost again. */
    if (ready_blocks.find(sdu_base) != ready_blocks.end())
    {
        return;
    }

    auto it = rx_blocks.find(sdu_base);
    if (it == rx_blocks.end())
    {
        while (rx_blocks.size() >= k_block_max)
        {
            auto oldest = rx_blocks.begin();
            for (auto j = rx_blocks.begin(); j != rx_blocks.end(); ++j)
            {
                if (j->second.last_seen < oldest->second.last_seen)
                {
                    oldest = j;
                }
            }
            const uint16_t evict_id = oldest->first;
            const rx_block_s block_snap = oldest->second;
            evicted_blocks_count++;
            rx_blocks.erase(oldest);
            abandon_partial_block(block_snap, evict_id, out);
        }
        rx_block_s nb;
        nb.k = k;
        nb.n = n;
        nb.sdu_n = sdu_n;
        const auto t_now = now();
        nb.first_seen = t_now;
        nb.last_seen = t_now;
        it = rx_blocks.emplace(sdu_base, std::move(nb)).first;
    }
    else if (it->second.k != k || it->second.n != n || it->second.sdu_n != sdu_n)
    {
        kn_mismatch_count++;
        const rx_block_s block_snap = it->second;
        rx_blocks.erase(it);
        abandon_partial_block(block_snap, sdu_base, out);
        return;
    }

    rx_block_s* buf = &it->second;
    if (buf->frags.count(index) != 0)
    {
        return;
    }
    buf->frags.emplace(index, std::move(shard));
    buf->last_seen = now();

    if (sdu_base == emit_next)
    {
        try_stream_head_systematic(*buf, out);
    }
    else if (dist_from_emit(sdu_base) > 0 && !later_block_waiting)
    {
        later_block_waiting = true;
        later_block_since = now();
    }

    if (try_decode_block(sdu_base, buf, out))
    {
        return;
    }

    run_emit_engine(out);
}

uint64_t vstreamer::rs_block_erasure::take_recovered()
{
    const uint64_t n = recovered_count - recovered_seen;
    recovered_seen = recovered_count;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_decode_fail()
{
    const uint64_t total = decode_fail();
    const uint64_t n = total - decode_fail_seen;
    decode_fail_seen = total;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_hdr_errors()
{
    const uint64_t n = hdr_errors_count - hdr_errors_seen;
    hdr_errors_seen = hdr_errors_count;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_kn_mismatch()
{
    const uint64_t n = kn_mismatch_count - kn_mismatch_seen;
    kn_mismatch_seen = kn_mismatch_count;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_evicted_blocks()
{
    const uint64_t n = evicted_blocks_count - evicted_blocks_seen;
    evicted_blocks_seen = evicted_blocks_count;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_rs_failures()
{
    const uint64_t n = rs_failures_count - rs_failures_seen;
    rs_failures_seen = rs_failures_count;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_fail_missing_shards()
{
    const uint64_t n = missing_shards_count - missing_shards_seen;
    missing_shards_seen = missing_shards_count;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_late_blocks()
{
    const uint64_t n = late_blocks_count - late_blocks_seen;
    late_blocks_seen = late_blocks_count;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_fail_lost_app_pkts()
{
    const uint64_t n = fail_lost_app_pkts_count - fail_lost_app_pkts_seen;
    fail_lost_app_pkts_seen = fail_lost_app_pkts_count;
    return n;
}
