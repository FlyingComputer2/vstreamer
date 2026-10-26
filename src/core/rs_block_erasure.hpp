#ifndef VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP
#define VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP

#include <chrono>
#include <deque>
#include <functional>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <bfc/shared_sized_buffer.hpp>

// Packet-block Reed-Solomon erasure FEC (systematic Cauchy MDS via ISA-L).
// k app datagrams become n on-air shards. Wire header is 5 bytes (see pack_header).
// n == k: no parity (non-FEC redundancy); same block framing and header on each shard.
// Wire (5 bytes before orig_len):
//   [0..1] sdu_base (u16 BE) — SDU seq of shard 0 in this block
//   [2..3] fec_config (u16 BE): k(5) | n(5) | idx(5) | spare(1), MSB-first
//   [4]    fec2: sdu_n(5) | spare(3), MSB-first; parity iff idx >= k
namespace vstreamer
{

using fec_rx_payload_list = std::vector<bfc::shared_sized_buffer>;

class rs_block_erasure
{
public:
    static constexpr size_t k_header_len = 5;
    static constexpr size_t k_len_prefix = 2;
    static constexpr uint16_t k_wire_fec_config_spare = 0x0001U;
    static constexpr uint8_t k_wire_fec2_spare = 0x07U;
    static constexpr int k_header_k_n_min = 1;
    static constexpr int k_header_k_n_max = 31;
    static constexpr int k_default_timeout_ms = 20;
    static constexpr size_t k_max_n = 255;
    static constexpr size_t k_done_max = 128;
    static constexpr size_t k_block_max = 256;

    /*
     * Receive sequencing. One reference: the next block id (sdu_base) to deliver. A shard whose
     * block id is behind it by at most k_max_lo_block_id_deviation is late and ignored; behind by
     * more, or ahead by more than k_max_hi_block_id_deviation, it starts a new reference (a new
     * sender session: old state is dropped). Ids count SDUs.
     *
     * Every message that can go out in sequence goes out at once: the head block's data shards
     * as they arrive, the rest when FEC recovers it, then the next blocks' ready prefixes. A head
     * block that cannot be completed is flushed (the data shards it has) at its deadline.
     *
     * Deadlines come only from receive times and the estimated sender spread: the sender
     * releases air position p of a c-shard block at t0 + p * spread / c, so each block's arrivals
     * are fitted against position (robust slope, Theil-Sen) and the per-block fits feed a running
     * spread estimate (weighted by shard count). A block's deadline is its fitted start plus
     * (c - 1) * spread / c plus a margin from the fit residuals. Until two blocks have been
     * fitted the spread is taken as k_spread_cap_ms (the sender's cap).
     */
    static constexpr int k_max_lo_block_id_deviation = 128;
    static constexpr int k_max_hi_block_id_deviation = 1024;
    static constexpr int k_spread_cap_ms = 250;
    /* Floor of the deadline margin: scheduling jitter on top of the measured arrival jitter. */
    static constexpr int k_deadline_margin_min_ms = 5;

    using clock_fn = std::function<std::chrono::steady_clock::time_point()>;
    /* Replaces steady_clock::now() (tests). */
    void set_clock(clock_fn fn)
    {
        clock = std::move(fn);
    }
    [[nodiscard]] bool have_spread_estimate() const
    {
        return fitted_blocks >= 2;
    }
    /* Estimated sender spread and arrival jitter (ms); the cap / 0 before an estimate. */
    [[nodiscard]] double spread_estimate_ms() const;
    [[nodiscard]] double jitter_estimate_ms() const;

    rs_block_erasure();

    // init() / disable() keep the TX sdu_seq counter running so a runtime
    // k/n change does not replay bases the receiver already saw.
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

    [[nodiscard]] uint16_t tx_sdu_seq() const
    {
        return sdu_seq;
    }
    // Overrides the random start of the TX sdu_seq; tests use it to cross the u16 wrap.
    void set_tx_sdu_seq(uint16_t seq)
    {
        sdu_seq = seq;
    }

    // App datagram -> zero or more air shards (full block or nothing).
    void push_app(const uint8_t* data, size_t len,
                  std::vector<std::vector<uint8_t>>* out);
    void flush(std::vector<std::vector<uint8_t>>* out);
    // TX tick: timeout-flush a partial block. Also expires RX state.
    void on_tick(std::vector<std::vector<uint8_t>>* out);
    /* Next partial-block flush time; false if no pending TX deadline. */
    [[nodiscard]] bool next_deadline(std::chrono::steady_clock::time_point* out) const;
    // RX tick: flush the head block once its deadline has passed and release what follows
    // in sequence. Call periodically even when no datagram arrives.
    void poll_rx(fec_rx_payload_list* out);

    // Air datagram (full 5-byte FEC shard header + body). Zero-copy RX stores the
    // buffer in the block; systematic emits are subviews of the shard body.
    void push_air(bfc::shared_sized_buffer shard, fec_rx_payload_list* out);
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
        return evicted_blocks_count + rs_failures_count + given_up_blocks_count;
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
    /* Blocks behind the reference that were never released here (each counted once); the
     * redundant shards of blocks already delivered are dropped silently. */
    uint64_t late_blocks() const
    {
        return late_blocks_count;
    }
    /* New references taken (sender session changes / out-of-window jumps). */
    uint64_t new_references() const
    {
        return new_references_count;
    }
    /* Head blocks flushed at their deadline with SDUs still missing. */
    uint64_t given_up_blocks() const
    {
        return given_up_blocks_count;
    }
    /* Time spent in the parity kernel (encode_block), microseconds. */
    uint64_t encode_kernel_us() const
    {
        return encode_kernel_us_;
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
                      uint16_t sdu_base,
                      std::vector<std::vector<uint8_t>>* out) const;
    // Decode from shard index -> body. k/n come from the wire header (or
    // from init() via the overload). Returns false if unrecoverable.
    bool decode_block(int k, int n, int sdu_n,
                      const std::unordered_map<int, bfc::shared_sized_buffer>& frags,
                      fec_rx_payload_list* payloads, int* recovered) const;
    bool decode_block(const std::unordered_map<int, bfc::shared_sized_buffer>& frags,
                      fec_rx_payload_list* payloads, int* recovered) const;

    [[nodiscard]] size_t max_original() const;

    static bool pack_header(uint8_t* out, uint16_t sdu_base, int index, int k, int n,
                            int sdu_n);
    static bool unpack_header(const uint8_t* data, size_t len, uint16_t* sdu_base,
                              int* index, int* k, int* n, int* sdu_n);

private:
    struct rx_block_s
    {
        int                                              k = 0;
        int                                              n = 0;
        int                                              sdu_n = 0;
        int                                              released = 0;
        std::unordered_map<int, bfc::shared_sized_buffer>     frags;
        /* (air position, arrival time) of every shard received, for the spread fit. */
        std::vector<std::pair<int, std::chrono::steady_clock::time_point>> arrivals;
        std::chrono::steady_clock::time_point            last_seen{};
    };

    struct ready_block_s
    {
        /* Empty buffer = message not recovered (RS failure); skipped on release. */
        fec_rx_payload_list payloads;
        int                               released = 0;
        int                               sdu_n = 0;
        /* Fitted time of the block's first air shard (gap deadlines behind it). */
        std::chrono::steady_clock::time_point start{};
    };

    void account_missing_shards(const rx_block_s& block);
    void note_rx_block_output_shortfall(int expected, int available);
    static int expected_sdus(const rx_block_s& block);

    static int ring_dist(uint16_t a, uint16_t b);
    int        dist_from_emit(uint16_t sdu_base) const;
    void       note_head_hole_sdus(int gap_sdus);

    void emit_payload(fec_rx_payload_list* out, bfc::shared_sized_buffer&& app);
    static bool frag_to_app(const bfc::shared_sized_buffer& shard, bfc::shared_sized_buffer* app);

    /* Air position of shard `index` (data shards first, then parity) and shards on air. */
    static int air_pos(int index, int k, int sdu_n);
    static int air_count(int k, int n, int sdu_n);
    std::chrono::nanoseconds spread_now() const;
    std::chrono::nanoseconds margin_now() const;
    std::chrono::steady_clock::time_point block_start(const rx_block_s& block) const;
    std::chrono::steady_clock::time_point block_deadline(const rx_block_s& block) const;
    /* Fits the block's arrivals and folds the result into the spread estimate. */
    void learn_spread(const rx_block_s& block);

    void new_reference(uint16_t sdu_base);
    /* Flushes the data shards the block has (in order) and returns how many SDUs are lost. */
    int abandon_partial_block(const rx_block_s& block, fec_rx_payload_list* out);
    void advance_head(int sdu_n);
    void note_released(uint16_t sdu_base);
    void try_stream_head_systematic(rx_block_s& block, fec_rx_payload_list* out);
    void run_emit_engine(fec_rx_payload_list* out);
    bool try_decode_block(uint16_t sdu_base, rx_block_s* block, fec_rx_payload_list* out);

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
    uint16_t sdu_seq = 0;

    std::unordered_map<uint16_t, rx_block_s> rx_blocks;
    std::unordered_map<uint16_t, ready_block_s> ready_blocks;
    static constexpr size_t                   k_recent_released_max = 64;
    std::deque<uint16_t>                      recent_released;
    bool                                      emit_base_set = false;
    uint16_t                                  emit_next = 0;

    clock_fn clock;
    /* Running spread / jitter estimate, seconds; valid once fitted_blocks >= 2. */
    double spread_est_s = 0.;
    double jitter_est_s = 0.;
    int    fitted_blocks = 0;

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
    uint64_t given_up_blocks_count = 0;
    uint64_t new_references_count = 0;
    uint64_t oversized_count = 0;
    mutable uint64_t encode_kernel_us_ = 0;

    size_t max_shard_bytes = 1470;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP
