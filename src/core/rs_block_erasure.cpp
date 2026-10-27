/* rs_block_erasure: wire header, TX encode and the block decoder (receive side is in _rx). */
#include "core/rs_block_erasure.hpp"
#include "core/rs_encode_scalar.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <utility>

#if defined(ESP_PLATFORM) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#if defined(CONFIG_VSTREAMER_RX_PSRAM) && CONFIG_VSTREAMER_RX_PSRAM
#include <new>

#include "esp_heap_caps.h"
#endif

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
using vstreamer::rs_block_erasure;

constexpr size_t k_min_shard = 16;  // ISA-L NEON kernels need >= 16 bytes.
constexpr size_t k_tx_body_align = 16;
constexpr int    k_max_k = rs_block_erasure::k_header_k_n_max;
constexpr int    k_max_rows = 2 * rs_block_erasure::k_header_k_n_max; /* data + parity */

void store_be16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

uint16_t load_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

size_t align_shard(size_t row)
{
    return row < k_min_shard ? k_min_shard : row;
}

/* Cauchy matrix of a block with k data rows and k + p rows in all; parity row j is row k + j. */
struct kn_encode_tables
{
    std::vector<uint8_t> encode_matrix;
    std::vector<uint8_t> g_tbls;
};

const kn_encode_tables& kn_encode_tables_for(int k, int total)
{
    static std::mutex mu;
    static std::array<std::array<std::optional<kn_encode_tables>, k_max_rows + 1>, k_max_k + 1>
                                cache;
    std::lock_guard<std::mutex> lock(mu);
    std::optional<kn_encode_tables>& slot =
        cache[static_cast<size_t>(k)][static_cast<size_t>(total)];
    if (!slot.has_value())
    {
        kn_encode_tables built;
        const int        p = total - k;
        built.encode_matrix.assign(static_cast<size_t>(total) * static_cast<size_t>(k), 0);
        built.g_tbls.assign(static_cast<size_t>(k) * static_cast<size_t>(std::max(p, 1)) * 32, 0);
        gf_gen_cauchy1_matrix(built.encode_matrix.data(), total, k);
        if (p > 0)
        {
            ec_init_tables_base(k, p, built.encode_matrix.data() + k * k, built.g_tbls.data());
        }
        slot = std::move(built);
    }
    return *slot;
}

/* Inverts the first k surviving rows. err_list[] is in row order, so its missing data rows come
 * first; decode_matrix gets one row for each of those (missing parity is never rebuilt). */
int gen_decode_matrix(int k, int total, const uint8_t* encode_matrix, const uint8_t* err_list,
                      int nerrs, uint8_t* decode_matrix, uint8_t* decode_index)
{
    std::array<uint8_t, k_max_rows> in_err {};
    for (int i = 0; i < nerrs; i++)
    {
        if (err_list[i] >= total)
        {
            return -1;
        }
        in_err[err_list[i]] = 1;
    }
    std::vector<uint8_t> b(static_cast<size_t>(k) * static_cast<size_t>(k));
    std::vector<uint8_t> invert(static_cast<size_t>(k) * static_cast<size_t>(k));
    int                  r = 0;
    for (int i = 0; i < k; i++, r++)
    {
        while (r < total && in_err[static_cast<size_t>(r)])
        {
            r++;
        }
        if (r >= total)
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
    for (int e = 0; e < nerrs && err_list[e] < k; e++)
    {
        memcpy(decode_matrix + static_cast<size_t>(k) * static_cast<size_t>(e),
               invert.data() + static_cast<size_t>(k) * static_cast<size_t>(err_list[e]),
               static_cast<size_t>(k));
    }
    return 0;
}

/* A 16-byte aligned decode row; in PSRAM where configured, to keep internal RAM for the
 * network and USB drivers. */
bfc::shared_sized_buffer alloc_decode_row(size_t len)
{
#if defined(CONFIG_VSTREAMER_RX_PSRAM) && CONFIG_VSTREAMER_RX_PSRAM
    auto* bytes = static_cast<std::byte*>(
        heap_caps_aligned_alloc(k_tx_body_align, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bytes == nullptr)
    {
        throw std::bad_alloc();
    }
    return bfc::shared_sized_buffer::adopt(bytes, len, len, [](std::byte* p) { heap_caps_free(p); });
#else
    auto storage = bfc::shared_sized_buffer::allocate(len + k_tx_body_align - 1);
    storage.resize(len + k_tx_body_align - 1);
    const size_t off = (k_tx_body_align - reinterpret_cast<uintptr_t>(storage.u8()) % k_tx_body_align) %
                       k_tx_body_align;
    return storage.subview(off, len);
#endif
}
}  // namespace

/* --- wire --- */

void vstreamer::rs_block_erasure::write_data_header(uint8_t* out, uint16_t sequence_number)
{
    out[0] = k_flag_is_data;
    store_be16(out + 1, sequence_number);
}

bool vstreamer::rs_block_erasure::write_parity_header(uint8_t* out, uint16_t sequence_number,
                                                      int n, int k_total, int k_i)
{
    if (out == nullptr || n < k_header_k_n_min || n > k_header_k_n_max ||
        k_total < k_header_k_n_min || k_total > k_header_k_n_max || k_i < 0 || k_i >= n)
    {
        return false;
    }
    const uint16_t flag = static_cast<uint16_t>(((static_cast<uint16_t>(n) & 0x1FU) << 10) |
                                                ((static_cast<uint16_t>(k_total) & 0x1FU) << 5) |
                                                (static_cast<uint16_t>(k_i) & 0x1FU));
    store_be16(out, flag);
    store_be16(out + 2, sequence_number);
    return true;
}

bool vstreamer::rs_block_erasure::parse_header(const uint8_t* data, size_t len, shard_header* out)
{
    if (data == nullptr || out == nullptr || len == 0)
    {
        return false;
    }
    shard_header h;
    if (0 != (data[0] & k_flag_is_data))
    {
        if (len <= k_data_header_len || 0 != (data[0] & k_data_flag_spare))
        {
            return false;
        }
        h.is_data = true;
        h.sequence_number = load_be16(data + 1);
        h.header_len = k_data_header_len;
        *out = h;
        return true;
    }
    if (len <= k_parity_header_len + k_len_prefix)
    {
        return false;
    }
    const uint16_t flag = load_be16(data);
    h.n = static_cast<int>((flag >> 10) & 0x1F);
    h.k_total = static_cast<int>((flag >> 5) & 0x1F);
    h.k_i = static_cast<int>(flag & 0x1F);
    if (h.n < k_header_k_n_min || h.k_total < k_header_k_n_min || h.k_i >= h.n)
    {
        return false;
    }
    h.sequence_number = load_be16(data + 2);
    h.header_len = k_parity_header_len;
    *out = h;
    return true;
}

/* --- setup --- */

vstreamer::rs_block_erasure::rs_block_erasure() : ring(k_rx_ring)
{
    // Random TX start id: a restarted sender must not replay ids that the receiver's in-order
    // release still remembers.
    std::random_device rd;
    tx_seq = static_cast<uint16_t>(rd());
}

const char* vstreamer::rs_block_erasure::impl_name() const
{
#ifdef VSTREAMER_ISAL_NEON
    return "isa-l neon";
#else
    return rs_encode_scalar_impl();
#endif
}

size_t vstreamer::rs_block_erasure::max_app_payload(size_t max_datagram)
{
    /* Parity is the largest shard: its header and the coded length prefix sit before a body as
     * long as the longest data payload. */
    const size_t overhead = k_parity_header_len + k_len_prefix;
    return max_datagram > overhead ? max_datagram - overhead : 0;
}

size_t vstreamer::rs_block_erasure::max_data_payload(size_t max_datagram)
{
    return max_datagram > k_data_header_len ? max_datagram - k_data_header_len : 0;
}

bool vstreamer::rs_block_erasure::init(int k, int n, size_t max_datagram)
{
    if (k < k_header_k_n_min || k > k_header_k_n_max || n < k || n - k > k_header_k_n_max ||
        max_datagram < k_parity_header_len + k_len_prefix + k_min_shard)
    {
        return false;
    }
    /* The open block keeps its old k/n: the caller flushes it first. */
    pending_rows.clear();
    cfg_k = k;
    cfg_n = n;
    p = n - k;
    max_datagram_ = max_datagram;
    full_g_tbls = kn_encode_tables_for(cfg_k, cfg_n).g_tbls.data();
    active = true;
    return true;
}

void vstreamer::rs_block_erasure::disable()
{
    active = false;
    pending_rows.clear();
    cfg_k = 0;
    cfg_n = 0;
    p = 0;
    full_g_tbls = nullptr;
}

void vstreamer::rs_block_erasure::set_tx_buffers(size_t headroom, shard_alloc alloc)
{
    tx_headroom = headroom;
    tx_alloc = std::move(alloc);
}

/* --- TX --- */

bool vstreamer::rs_block_erasure::alloc_tx_row(tx_row* row, size_t header_len, size_t body_max)
{
    const size_t cap = k_tx_body_align - 1 + tx_headroom + header_len + body_max;
    if (tx_alloc)
    {
        row->buf = tx_alloc(cap);
    }
    else
    {
        row->buf = bfc::shared_sized_buffer::allocate(cap);
        row->buf.resize(cap);
    }
    if (row->buf.size() < cap)
    {
        row->buf = {};
        return false;
    }
    const auto body = reinterpret_cast<uintptr_t>(row->buf.u8()) + tx_headroom + header_len;
    row->start = (k_tx_body_align - body % k_tx_body_align) % k_tx_body_align;
    return true;
}

void vstreamer::rs_block_erasure::push_app(const uint8_t* data, size_t len, tx_out* out)
{
    if (out == nullptr)
    {
        return;
    }
    out->shards.clear();
    out->parity = 0;
    out->block_k = 0;
    if (!active || data == nullptr)
    {
        return;
    }
    const size_t body_max = max_original();
    tx_row       row;
    if (0 == len || len > body_max || !alloc_tx_row(&row, k_data_header_len, body_max))
    {
        oversized_count++;
        return;
    }
    uint8_t* hdr = row.buf.u8() + row.start + tx_headroom;
    write_data_header(hdr, tx_seq);
    memcpy(hdr + k_data_header_len, data, len);
    if (p > 0)
    {
        /* The kernel reads every row to the block's longest; zero the tail now, before the shard
         * is handed out, so nothing writes into it while it is in flight. */
        memset(hdr + k_data_header_len + len, 0, body_max - len);
    }
    row.len = len;
    if (pending_rows.empty())
    {
        block_base = tx_seq;
        block_start_ = clock ? clock() : std::chrono::steady_clock::now();
    }
    tx_seq++;
    out->shards.push_back(row.buf.subview(row.start, tx_headroom + k_data_header_len + len));
    if (0 == p)
    {
        return;
    }
    pending_rows.push_back(std::move(row));
    if (static_cast<int>(pending_rows.size()) >= cfg_k)
    {
        (void)encode_block(out);
    }
}

void vstreamer::rs_block_erasure::flush(tx_out* out)
{
    if (out == nullptr)
    {
        return;
    }
    out->shards.clear();
    out->parity = 0;
    out->block_k = 0;
    if (!active || pending_rows.empty())
    {
        return;
    }
    (void)encode_block(out);
}

/* Parity over pending_rows, appended to out->shards. Two kernel passes: the data bodies (in place,
 * already zero-padded) and the 2-byte length rows; parity is [length parity | data parity]. */
bool vstreamer::rs_block_erasure::encode_block(tx_out* out)
{
    const int k_total = static_cast<int>(pending_rows.size());
    size_t    max_len = 0;
    for (const tx_row& row : pending_rows)
    {
        max_len = std::max(max_len, row.len);
    }
    const size_t shard_len = align_shard(max_len);
    const size_t body_max = max_original();
    if (shard_len > body_max || k_total < 1 || k_total > k_header_k_n_max)
    {
        oversized_count++;
        pending_rows.clear();
        return false;
    }
    /* The cache never moves an entry, so the full block's tables can be kept from init(). */
    unsigned char* const g_tbls = const_cast<unsigned char*>(
        k_total == cfg_k && nullptr != full_g_tbls
            ? full_g_tbls
            : kn_encode_tables_for(k_total, k_total + p).g_tbls.data());

    alignas(16) uint8_t len_rows[k_max_k][k_min_shard] {};
    alignas(16) uint8_t len_parity[k_max_k][k_min_shard] {};
    std::array<unsigned char*, k_max_k> data_ptrs {};
    std::array<unsigned char*, k_max_k> len_ptrs {};
    std::array<unsigned char*, k_max_k> coding_ptrs {};
    std::array<unsigned char*, k_max_k> len_coding_ptrs {};
    const size_t                        data_off = tx_headroom + k_data_header_len;
    for (int i = 0; i < k_total; i++)
    {
        tx_row& row = pending_rows[static_cast<size_t>(i)];
        data_ptrs[static_cast<size_t>(i)] = row.buf.u8() + row.start + data_off;
        store_be16(len_rows[i], static_cast<uint16_t>(row.len));
        len_ptrs[static_cast<size_t>(i)] = len_rows[i];
    }
    std::vector<tx_row> parity(static_cast<size_t>(p));
    const size_t        parity_off = tx_headroom + k_parity_header_len + k_len_prefix;
    for (int j = 0; j < p; j++)
    {
        tx_row& row = parity[static_cast<size_t>(j)];
        if (!alloc_tx_row(&row, k_parity_header_len + k_len_prefix, body_max))
        {
            oversized_count++;
            pending_rows.clear();
            return false;
        }
        coding_ptrs[static_cast<size_t>(j)] = row.buf.u8() + row.start + parity_off;
        len_coding_ptrs[static_cast<size_t>(j)] = len_parity[j];
    }
    const auto kernel_t0 = std::chrono::steady_clock::now();
    VSTREAMER_EC_ENCODE(static_cast<int>(shard_len), k_total, p, g_tbls, data_ptrs.data(),
                        coding_ptrs.data());
    VSTREAMER_EC_ENCODE(static_cast<int>(k_min_shard), k_total, p, g_tbls, len_ptrs.data(),
                        len_coding_ptrs.data());
    encode_kernel_us_ += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                              kernel_t0)
            .count());

    for (int j = 0; j < p; j++)
    {
        tx_row&  row = parity[static_cast<size_t>(j)];
        uint8_t* hdr = row.buf.u8() + row.start + tx_headroom;
        (void)write_parity_header(hdr, block_base, p, k_total, j);
        memcpy(hdr + k_parity_header_len, len_parity[j], k_len_prefix);
        out->shards.push_back(row.buf.subview(
            row.start, tx_headroom + k_parity_header_len + k_len_prefix + shard_len));
    }
    out->parity = p;
    out->block_k = k_total;
    out->block_start = block_start_;
    pending_rows.clear();
    blocks_count++;
    return true;
}

/* --- decode --- */

bool vstreamer::rs_block_erasure::decode_rows(int k_total, int n,
                                              std::vector<bfc::shared_sized_buffer>* data,
                                              const std::vector<bfc::shared_sized_buffer>& parity,
                                              int* recovered)
{
    if (data == nullptr || k_total < 1 || k_total > k_header_k_n_max || n < 1 ||
        n > k_header_k_n_max || data->size() != static_cast<size_t>(k_total) ||
        parity.size() != static_cast<size_t>(n))
    {
        return false;
    }
    const int total = k_total + n;
    /* Row length: every parity body has it; data rows are [len | payload] zero-padded to it. */
    size_t row_len = 0;
    int    have = 0;
    for (const auto& body : parity)
    {
        if (!body.empty())
        {
            if (row_len != 0 && body.size() != row_len)
            {
                return false;
            }
            row_len = body.size();
            have++;
        }
    }
    int missing_data = 0;
    for (const auto& payload : *data)
    {
        if (payload.empty())
        {
            missing_data++;
        }
        else
        {
            have++;
        }
    }
    if (recovered != nullptr)
    {
        *recovered = 0;
    }
    if (0 == missing_data)
    {
        return true;
    }
    if (have < k_total || row_len < k_len_prefix + k_min_shard)
    {
        return false;
    }
    for (const auto& payload : *data)
    {
        if (!payload.empty() && k_len_prefix + payload.size() > row_len)
        {
            return false;
        }
    }

    const kn_encode_tables&         tables = kn_encode_tables_for(k_total, total);
    std::array<uint8_t, k_max_rows> err_list {};
    int                             nerrs = 0;
    for (int i = 0; i < total; i++)
    {
        const bool missing = i < k_total ? (*data)[static_cast<size_t>(i)].empty()
                                         : parity[static_cast<size_t>(i - k_total)].empty();
        if (missing)
        {
            err_list[static_cast<size_t>(nerrs++)] = static_cast<uint8_t>(i);
        }
    }
    /* Missing data rows are the head of err_list (row order). */
    std::vector<uint8_t> decode_matrix(static_cast<size_t>(missing_data) *
                                       static_cast<size_t>(k_total));
    std::array<uint8_t, k_max_rows> decode_index {};
    if (gen_decode_matrix(k_total, total, tables.encode_matrix.data(), err_list.data(), nerrs,
                          decode_matrix.data(), decode_index.data()) != 0)
    {
        return false;
    }
    /* Only the k source rows the decoder picked are read. A parity body already is a row; a data
     * row is built as [u16 BE length | payload | zero pad]. */
    std::array<bfc::shared_sized_buffer, k_max_k> data_rows {};
    std::array<unsigned char*, k_max_rows>        src_ptrs {};
    for (int i = 0; i < k_total; i++)
    {
        const int idx = decode_index[static_cast<size_t>(i)];
        if (idx >= k_total)
        {
            src_ptrs[static_cast<size_t>(i)] =
                const_cast<unsigned char*>(parity[static_cast<size_t>(idx - k_total)].u8());
            continue;
        }
        const bfc::shared_sized_buffer& src = (*data)[static_cast<size_t>(idx)];
        bfc::shared_sized_buffer&       row = data_rows[static_cast<size_t>(idx)];
        row = alloc_decode_row(row_len);
        store_be16(row.u8(), static_cast<uint16_t>(src.size()));
        memcpy(row.u8() + k_len_prefix, src.u8(), src.size());
        memset(row.u8() + k_len_prefix + src.size(), 0, row_len - k_len_prefix - src.size());
        src_ptrs[static_cast<size_t>(i)] = row.u8();
    }
    std::vector<uint8_t> decode_tbls(static_cast<size_t>(k_total) *
                                     static_cast<size_t>(missing_data) * 32);
    std::array<bfc::shared_sized_buffer, k_max_k> out_rows {};
    std::array<unsigned char*, k_max_rows>        out_ptrs {};
    for (int w = 0; w < missing_data; w++)
    {
        out_rows[static_cast<size_t>(w)] = alloc_decode_row(row_len);
        out_ptrs[static_cast<size_t>(w)] = out_rows[static_cast<size_t>(w)].u8();
    }
    ec_init_tables_base(k_total, missing_data, decode_matrix.data(), decode_tbls.data());
    VSTREAMER_EC_ENCODE(static_cast<int>(row_len), k_total, missing_data, decode_tbls.data(),
                        src_ptrs.data(), out_ptrs.data());
    for (int w = 0; w < missing_data; w++)
    {
        const bfc::shared_sized_buffer& row = out_rows[static_cast<size_t>(w)];
        const uint16_t                  len = load_be16(row.u8());
        if (0 == len || k_len_prefix + len > row_len)
        {
            return false;
        }
    }
    for (int w = 0; w < missing_data; w++)
    {
        (*data)[err_list[static_cast<size_t>(w)]] =
            out_rows[static_cast<size_t>(w)].subview(k_len_prefix,
                                                     load_be16(out_rows[static_cast<size_t>(w)].u8()));
    }
    if (recovered != nullptr)
    {
        *recovered = missing_data;
    }
    return true;
}

/* --- counters --- */

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

uint64_t vstreamer::rs_block_erasure::take_fail_missing_shards()
{
    const uint64_t total = missing_shards();
    const uint64_t n = total - missing_shards_seen;
    missing_shards_seen = total;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_fail_lost_app_pkts()
{
    const uint64_t n = missing_data_count - lost_app_seen;
    lost_app_seen = missing_data_count;
    return n;
}

uint64_t vstreamer::rs_block_erasure::take_missing_parity()
{
    const uint64_t n = missing_parity_count - missing_parity_seen;
    missing_parity_seen = missing_parity_count;
    return n;
}
