#ifndef VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP
#define VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP

#include <chrono>
#include <deque>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/shared_sized_buffer.hpp"

// Packet-block Reed-Solomon erasure FEC (systematic Cauchy MDS via ISA-L).
// k app datagrams become n on-air shards. Wire header is 4 bytes (see pack_header).
// n == k: no parity (non-FEC redundancy); same block framing and header on each shard.
// Wire (4 bytes, 32 bits):
//   [0] block_id (8)
//   [1] parity(1) | reserved(3) | shard_index(4)   — index < n <= 15
//   [2] n(4) | k(4)
//   [3] reserved(4) | sdu_n(4)                     — sdu_n <= k
// Spare for extensions: 8 bits if parity is derived (index >= k); we still
// set the parity bit on TX and require byte1 bits 6..4 and byte3 bits 7..4
// zero on RX (7 bits reserved today; the parity bit is the 8th logical spare).
namespace vstreamer
{

using fec_rx_payload_list = std::vector<shared_sized_buffer>;

class rs_block_erasure
{
public:
    static constexpr size_t k_header_len = 4;
    static constexpr size_t k_len_prefix = 2;
    static constexpr uint8_t k_flag_parity = 0x80;          /* bit 7 of byte 1 */
    static constexpr uint8_t k_wire_index_mask = 0x0F;      /* byte 1 bits 3..0 */
    static constexpr uint8_t k_wire_index_reserved = 0x70; /* byte 1 bits 6..4 */
    static constexpr uint8_t k_wire_sdu_n_reserved = 0xF0;  /* byte 3 bits 7..4 */
    static constexpr int k_header_k_n_min = 1;
    static constexpr int k_header_k_n_max = 15; /* 4-bit k and n on wire */
    static constexpr int k_max_wire_n = k_header_k_n_max;
    static constexpr int k_default_timeout_ms = 20;
    static constexpr size_t k_max_n = 255;
    static constexpr size_t k_done_max = 128;
    static constexpr size_t k_block_max = 256;
    /* block_id on the 4-byte shard header is one byte; emit/dedupe use this ring. */
    static constexpr uint16_t k_wire_block_id_mod = 256;

    [[nodiscard]] static constexpr uint16_t wire_block_id(uint16_t id)
    {
        return static_cast<uint16_t>(id & (k_wire_block_id_mod - 1U));
    }

    // Incomplete RX block TTL, and finished-id TTL (duplicate-shard
    // suppression). done_hold is longer so a late shard of a completed block
    // is still dropped, but short enough that a peer restart which reuses
    // block_id from 0 is accepted after the old ids age out.
    int rx_hold_ms() const;
    int done_hold_ms() const;
    // Max time a decoded block waits in the in-order emit queue for an
    // earlier block_id before the queue advances past the hole.
    int emit_hold_ms() const;

    rs_block_erasure();

    // init() / disable() keep the TX block_id counter running so a runtime
    // k/n change does not replay ids the receiver already saw.
    bool init(int k, int n, int timeout_ms, size_t max_shard_bytes = 1470);
    // Stop TX encode; flush pending first via flush()/announce_down. RX decode
    // still works from shard headers.
    void disable();
    bool enabled() const
    {
        return active;
    }
    int k() const
    {
        return cfg_k;
    }
    int n() const
    {
        return cfg_n;
    }
    const char* impl_name() const;

    // App datagram -> zero or more air shards (full block or nothing).
    void push_app(const uint8_t* data, size_t len,
                  std::vector<std::vector<uint8_t>>* out);
    void flush(std::vector<std::vector<uint8_t>>* out);
    // TX tick: timeout-flush a partial block. Also expires RX state.
    void on_tick(std::vector<std::vector<uint8_t>>* out);
    /* Next partial-block flush time; false if no pending TX deadline. */
    [[nodiscard]] bool next_deadline(std::chrono::steady_clock::time_point* out) const;
    // RX tick: expire stale blocks and release payloads held in the
    // in-order emit queue whose head-of-line wait has elapsed. Call
    // periodically even when no datagram arrives.
    void poll_rx(fec_rx_payload_list* out);

    // Air datagram (full 4-byte FEC shard header + body). Zero-copy RX stores the
    // buffer in the block; systematic emits are subviews of the shard body.
    void push_air(shared_sized_buffer shard, fec_rx_payload_list* out);
    void push_air(const uint8_t* data, size_t len, fec_rx_payload_list* out);

    uint64_t recovered() const
    {
        return recovered_count;
    }
    uint64_t blocks() const
    {
        return blocks_count;
    }
    uint64_t decode_fail() const
    {
        return evicted_blocks_count + rs_failures_count;
    }
    uint64_t hdr_errors() const
    {
        return hdr_errors_count;
    }
    uint64_t kn_mismatch() const
    {
        return kn_mismatch_count;
    }
    uint64_t evicted_blocks() const
    {
        return evicted_blocks_count;
    }
    uint64_t rs_failures() const
    {
        return rs_failures_count;
    }
    uint64_t missing_shards() const
    {
        return missing_shards_count;
    }
    uint64_t late_blocks() const
    {
        return late_blocks_count;
    }
    uint64_t oversized() const
    {
        return oversized_count;
    }
    // Interval since last take. Lifetime totals stay in recovered() / decode_fail() / …
    uint64_t take_recovered();
    uint64_t take_decode_fail();
    uint64_t take_hdr_errors();
    uint64_t take_kn_mismatch();
    uint64_t take_evicted_blocks();
    uint64_t take_rs_failures();
    /* Shards still missing when an RX block is evicted / give-up (interval take). */
    uint64_t take_fail_missing_shards();
    uint64_t take_late_blocks();
    uint64_t take_fail_lost_app_pkts();

    // Encode one block. packets.size() may be < k (tail slots are virtual pads).
    // Empty systematic slots (index >= packets.size()) are not sent; the receiver
    // inserts zero-length bodies before decode. Non-empty systematic shards omit
    // trailing zeros on the wire; parity is full width.
    bool encode_block(const std::vector<std::vector<uint8_t>>& packets,
                      uint16_t block_id,
                      std::vector<std::vector<uint8_t>>* out) const;
    // Decode from shard index -> body. k/n come from the wire header (or
    // from init() via the overload). Returns false if unrecoverable.
    bool decode_block(int k, int n, int sdu_n,
                      const std::unordered_map<int, shared_sized_buffer>& frags,
                      fec_rx_payload_list* payloads, int* recovered) const;
    bool decode_block(const std::unordered_map<int, shared_sized_buffer>& frags,
                      fec_rx_payload_list* payloads, int* recovered) const;

    [[nodiscard]] size_t max_original() const;

    static bool pack_header(uint8_t* out, uint16_t block_id, int index, int k, int n,
                            uint8_t flags, int sdu_n);
    static bool unpack_header(const uint8_t* data, size_t len, uint16_t* block_id,
                              int* index, int* k, int* n, uint8_t* flags, int* sdu_n);

private:
    static constexpr int k_ring_evict_dist = 64;

    struct rx_block_s
    {
        int                                              k = 0;
        int                                              n = 0;
        int                                              sdu_n = 0;
        int                                              released = 0;
        std::unordered_map<int, shared_sized_buffer>     frags;
        std::chrono::steady_clock::time_point            first_seen{};
        std::chrono::steady_clock::time_point            last_seen{};
    };

    struct ready_block_s
    {
        fec_rx_payload_list payloads;
        int                               released = 0;
        int                               sdu_n = 0;
    };

    void account_missing_shards(const rx_block_s& block);
    void note_rx_block_output_shortfall(int expected, int available);
    static int expected_sdus(const rx_block_s& block);

    static int ring_dist(uint8_t a, uint8_t b);
    int        dist_from_emit(uint16_t block_id) const;

    void record_payload_emit();
    void emit_payload(fec_rx_payload_list* out, shared_sized_buffer&& app);
    static bool frag_to_app(const shared_sized_buffer& shard, shared_sized_buffer* app);

    void abandon_partial_block(const rx_block_s& block, uint16_t block_id, fec_rx_payload_list* out);
    void expire_rx(fec_rx_payload_list* out);
    void expire_done();
    void mark_done(uint16_t block_id);
    void note_emit_base(uint16_t block_id);
    void touch_newest(uint16_t block_id);
    void ring_evict_stale(fec_rx_payload_list* out);
    void maybe_resync_on_late_shard(uint16_t block_id);
    void maybe_rebase_after_silence(uint16_t block_id);
    void clear_state_behind(uint16_t base_id);
    void try_stream_head_systematic(rx_block_s& block, fec_rx_payload_list* out);
    void on_block_decoded(uint16_t block_id, int released_before, fec_rx_payload_list payloads,
                          int sdu_n, fec_rx_payload_list* out);
    void maybe_give_up_head(fec_rx_payload_list* out);
    void run_emit_engine(fec_rx_payload_list* out);
    bool try_decode_block(uint16_t block_id, rx_block_s* block, fec_rx_payload_list* out);

    std::chrono::steady_clock::time_point now() const;
    static int gen_decode_matrix(int k, int n, const uint8_t* encode_matrix,
                                 const uint8_t* err_list, int nerrs,
                                 uint8_t* decode_matrix, uint8_t* decode_index);

    bool active = false;
    int  cfg_k = 0;
    int  cfg_n = 0;
    int  p = 0;
    int  timeout_ms = k_default_timeout_ms;
    std::vector<uint8_t> encode_matrix;
    std::vector<uint8_t> g_tbls;

    std::vector<std::vector<uint8_t>> pending;
    std::chrono::steady_clock::time_point deadline{};
    bool deadline_set = false;
    uint16_t block_id = 0;

    std::unordered_map<uint16_t, rx_block_s> rx_blocks;
    std::deque<uint16_t> done_order;
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> done;

    std::unordered_map<uint16_t, ready_block_s> ready_blocks;
    bool                                      emit_base_set = false;
    uint16_t                                  emit_next = 0;
    uint8_t                                   newest = 0;
    bool                                      newest_set = false;
    std::chrono::steady_clock::time_point     last_payload_emit{};
    bool                                      have_payload_emit = false;
    std::chrono::steady_clock::time_point     last_shard_rx{};
    bool                                      have_shard_rx = false;
    std::chrono::steady_clock::time_point     later_block_since{};
    bool                                      later_block_waiting = false;

    uint64_t recovered_count = 0;
    uint64_t recovered_seen = 0;
    uint64_t blocks_count = 0;
    uint64_t decode_fail_seen = 0;
    uint64_t hdr_errors_count = 0;
    uint64_t hdr_errors_seen = 0;
    uint64_t kn_mismatch_count = 0;
    uint64_t kn_mismatch_seen = 0;
    uint64_t evicted_blocks_count = 0;
    uint64_t evicted_blocks_seen = 0;
    uint64_t rs_failures_count = 0;
    uint64_t rs_failures_seen = 0;
    uint64_t missing_shards_count = 0;
    uint64_t missing_shards_seen = 0;
    uint64_t fail_lost_app_pkts_count = 0;
    uint64_t fail_lost_app_pkts_seen = 0;
    uint64_t late_blocks_count = 0;
    uint64_t late_blocks_seen = 0;
    uint64_t oversized_count = 0;

    size_t max_shard_bytes = 1470;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP
