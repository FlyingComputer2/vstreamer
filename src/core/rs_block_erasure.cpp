#include "core/rs_block_erasure.hpp"
#include "core/rs_encode_scalar.hpp"

#include <chrono>

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
#define VSTREAMER_EC_ENCODE vstreamer::rs_encode_scalar
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
    return rs_encode_scalar_impl();
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
    pending_rows.clear();
    deadline_set = false;
    rx_blocks.clear();
    ready_blocks.clear();
    emit_base_set = false;
    emit_next = 0;
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
    given_up_blocks_count = 0;
    new_references_count = 0;
    oversized_count = 0;
    active = true;
    return true;
}

void vstreamer::rs_block_erasure::disable()
{
    active = false;
    pending.clear();
    pending_rows.clear();
    deadline_set = false;
    cfg_k = 0;
    cfg_n = 0;
    p = 0;
    encode_matrix.clear();
    g_tbls.clear();
    rx_blocks.clear();
    ready_blocks.clear();
    emit_base_set = false;
    emit_next = 0;
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
        const auto kernel_t0 = std::chrono::steady_clock::now();
        VSTREAMER_EC_ENCODE(static_cast<int>(shard_len), cfg_k, p,
                          const_cast<unsigned char*>(g_tbls.data()),
                          data_ptrs.data(), coding_ptrs.data());
        encode_kernel_us_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - kernel_t0)
                .count());
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
    const std::unordered_map<int, bfc::shared_sized_buffer>& frags, fec_rx_payload_list* payloads,
    int* recovered) const
{
    if (!active)
    {
        return false;
    }
    return decode_block(cfg_k, cfg_n, cfg_k, frags, payloads, recovered);
}

bool vstreamer::rs_block_erasure::decode_block(
    int k, int n, int sdu_n, const std::unordered_map<int, bfc::shared_sized_buffer>& frags,
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
        const bfc::shared_sized_buffer& shard = frags.at(idx);
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
                bfc::shared_sized_buffer app;
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
                bfc::shared_sized_buffer::copy_from(row.data() + k_len_prefix, orig_len));
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
        payloads->push_back(bfc::shared_sized_buffer::copy_from(row.data() + k_len_prefix, orig_len));
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

void vstreamer::rs_block_erasure::set_tx_buffers(size_t headroom, shard_alloc alloc)
{
    tx_headroom = headroom;
    tx_alloc = std::move(alloc);
}

namespace
{
constexpr size_t k_tx_body_align = 16;
}

bool vstreamer::rs_block_erasure::alloc_tx_row(tx_row* row)
{
    if (!tx_alloc)
    {
        return false;
    }
    const size_t shard_max = max_shard_bytes - k_header_len;
    const size_t cap = k_tx_body_align - 1 + tx_headroom + k_header_len + shard_max;
    row->buf = tx_alloc(cap);
    if (row->buf.size() < cap)
    {
        row->buf = {};
        return false;
    }
    const auto body = reinterpret_cast<uintptr_t>(row->buf.u8()) + tx_headroom + k_header_len;
    row->start = (k_tx_body_align - body % k_tx_body_align) % k_tx_body_align;
    return true;
}

void vstreamer::rs_block_erasure::push_app(const uint8_t* data, size_t len,
                                           std::vector<bfc::shared_sized_buffer>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!active || out == nullptr || data == nullptr)
    {
        return;
    }
    tx_row row;
    if (len > max_original() || !alloc_tx_row(&row))
    {
        oversized_count++;
        return;
    }
    uint8_t* body = row.buf.u8() + row.start + tx_headroom + k_header_len;
    store_be16(body, static_cast<uint16_t>(len));
    memcpy(body + k_len_prefix, data, len);
    row.len = len;
    pending_rows.push_back(std::move(row));
    if (!deadline_set)
    {
        deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
        deadline_set = true;
    }
    if (static_cast<int>(pending_rows.size()) >= cfg_k)
    {
        flush(out);
    }
}

void vstreamer::rs_block_erasure::flush(std::vector<bfc::shared_sized_buffer>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
    if (!active || out == nullptr || pending_rows.empty())
    {
        return;
    }
    const int sdu_n = static_cast<int>(pending_rows.size());
    if (!encode_tx_rows(sdu_seq, out))
    {
        oversized_count++;
        out->clear();
        pending_rows.clear();
        deadline_set = false;
        return;
    }
    sdu_seq = static_cast<uint16_t>(sdu_seq + static_cast<uint16_t>(sdu_n));
    pending_rows.clear();
    deadline_set = false;
    blocks_count++;
}

void vstreamer::rs_block_erasure::on_tick(std::vector<bfc::shared_sized_buffer>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
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

/* encode_block() over pending_rows, in place: same shard bytes, same order. */
bool vstreamer::rs_block_erasure::encode_tx_rows(uint16_t sdu_base,
                                                 std::vector<bfc::shared_sized_buffer>* out)
{
    const int sdu_n = static_cast<int>(pending_rows.size());
    if (sdu_n > cfg_k)
    {
        return false;
    }
    size_t max_row = k_len_prefix;
    for (const tx_row& row : pending_rows)
    {
        max_row = std::max(max_row, k_len_prefix + row.len);
    }
    const size_t shard_len = align_shard(max_row);
    if (k_header_len + shard_len > max_shard_bytes)
    {
        return false;
    }
    if (tx_zero_row.size() < shard_len)
    {
        tx_zero_row.assign(shard_len, 0);
    }
    const size_t body_off = tx_headroom + k_header_len;
    std::array<unsigned char*, k_header_k_n_max> data_ptrs {}; /* k, n <= 31: header fields */
    std::array<unsigned char*, k_header_k_n_max> coding_ptrs {};
    for (int i = 0; i < cfg_k; i++)
    {
        if (i < sdu_n)
        {
            tx_row& row = pending_rows[static_cast<size_t>(i)];
            uint8_t* body = row.buf.u8() + row.start + body_off;
            /* Short rows are zero-padded to shard_len for the kernel; the pad is not sent. */
            memset(body + k_len_prefix + row.len, 0, shard_len - k_len_prefix - row.len);
            data_ptrs[static_cast<size_t>(i)] = body;
        }
        else
        {
            data_ptrs[static_cast<size_t>(i)] = tx_zero_row.data();
        }
    }
    std::vector<tx_row> parity(static_cast<size_t>(p));
    for (int i = 0; i < p; i++)
    {
        tx_row& row = parity[static_cast<size_t>(i)];
        if (!alloc_tx_row(&row))
        {
            return false;
        }
        coding_ptrs[static_cast<size_t>(i)] = row.buf.u8() + row.start + body_off;
    }
    if (p > 0)
    {
        const auto kernel_t0 = std::chrono::steady_clock::now();
        VSTREAMER_EC_ENCODE(static_cast<int>(shard_len), cfg_k, p,
                          const_cast<unsigned char*>(g_tbls.data()), data_ptrs.data(),
                          coding_ptrs.data());
        encode_kernel_us_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - kernel_t0)
                .count());
    }

    out->clear();
    out->reserve(static_cast<size_t>(sdu_n + p));
    for (int i = 0; i < cfg_n; i++)
    {
        if (i < cfg_k && i >= sdu_n)
        {
            continue;
        }
        tx_row&      row = i < cfg_k ? pending_rows[static_cast<size_t>(i)]
                                     : parity[static_cast<size_t>(i - cfg_k)];
        const size_t body_len = i < cfg_k ? k_len_prefix + row.len : shard_len;
        pack_header(row.buf.u8() + row.start + tx_headroom, sdu_base, i, cfg_k, cfg_n, sdu_n);
        out->push_back(row.buf.subview(row.start, body_off + body_len));
    }
    return true;
}

void vstreamer::rs_block_erasure::on_tick(std::vector<std::vector<uint8_t>>* out)
{
    if (out != nullptr)
    {
        out->clear();
    }
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
