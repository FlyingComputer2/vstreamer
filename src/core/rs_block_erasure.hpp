#ifndef VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP
#define VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP

#include <chrono>
#include <functional>
#include <map>
#include <stddef.h>
#include <stdint.h>
#include <vector>

#include <bfc/shared_sized_buffer.hpp>

/*
 * Streaming Reed-Solomon erasure FEC (systematic Cauchy MDS via ISA-L, or the scalar / PIE
 * kernels). Every app datagram leaves at once as a data shard; every k data shards close a block
 * and add p parity shards that cover them. Nothing waits for a block to fill.
 *
 * Wire (bits MSB-first):
 *   data shard:    u8  flag = is_data(1)=1 | spare(7)=0
 *                  u16 sequence_number (BE): one counter over data shards only
 *                  data (its length is the datagram's)
 *   parity shard:  u16 flag (BE) = is_data(1)=0 | n(5) | k_total(5) | k_i(5)
 *                  u16 sequence_number (BE): the block's first data shard
 *                  parity
 * n is the block's parity count, k_total its data count, k_i this parity's row (0..n-1). The
 * block covers data [sequence_number, sequence_number + k_total).
 *
 * Parity is computed over rows of [u16 BE data length | data | zero pad] padded to the block's
 * longest row, so a recovered row carries its own length; the length prefix is never sent with
 * a data shard. Parity rows start at that length prefix.
 */
namespace vstreamer
{

using fec_rx_payload_list = std::vector<bfc::shared_sized_buffer>;

class rs_block_erasure
{
public:
    static constexpr size_t   k_data_header_len = 3;
    static constexpr size_t   k_parity_header_len = 4;
    static constexpr size_t   k_len_prefix = 2;
    static constexpr uint8_t  k_flag_is_data = 0x80U;
    static constexpr uint8_t  k_data_flag_spare = 0x7FU;
    static constexpr uint16_t k_parity_flag_is_data = 0x8000U;
    static constexpr int      k_header_k_n_min = 1;
    static constexpr int      k_header_k_n_max = 31;
    /* Ids: a data shard behind the next id to release by at most k_max_lo_seq_deviation is late
     * and ignored; behind by more, or ahead by more than k_max_hi_seq_deviation, it starts a new
     * reference (a new sender session: old state is dropped). */
    static constexpr int k_max_lo_seq_deviation = 128;
    static constexpr int k_max_hi_seq_deviation = 1024;
    /* RX ring of data shards (power of two, > hi deviation + the longest block). */
    static constexpr size_t k_rx_ring = 2048;
    static constexpr size_t k_block_max = 256;
    /*
     * Give-up. The sender releases parity row j of a block of c shards at
     * max(close + j*spread/c, first + (k_total + j)*spread/c), where close and first are when its
     * last and first data shards were pushed: a block that filled in a burst spreads its parity
     * over the whole spread, one that filled slowly sends it right after the close. The receiver
     * learns the spread from the row gap of each block's parity arrivals, and the bias and jitter
     * of the last parity's arrival against that rule. A missing data shard is given up when its
     * block's last parity is due (from the block's own fill time) + bias + a margin from the
     * jitter. Until two blocks have been measured the wait is k_spread_cap_ms (the sender's
     * spread cap) after the close. Without parity (a data-only stream) a hole is given up as
     * soon as a later data shard shows the sender is past it.
     */
    static constexpr int k_spread_cap_ms = 250;
    static constexpr int k_deadline_margin_min_ms = 5;
    /* Data shards without any parity after which the stream counts as data-only. Spread parity
     * can trail its block by many data shards, so this is well above the largest block. */
    static constexpr int k_data_only_after = 256;
    /* The same before any parity has been seen in this session: two of the largest blocks. */
    static constexpr int k_data_only_before_parity = 2 * k_header_k_n_max;

    struct shard_header
    {
        bool     is_data = false;
        uint16_t sequence_number = 0;
        int      n = 0;       /* parity only: parity count */
        int      k_total = 0; /* parity only: data count */
        int      k_i = 0;     /* parity only: row */
        size_t   header_len = 0;
    };
    static void write_data_header(uint8_t* out, uint16_t sequence_number);
    static bool write_parity_header(uint8_t* out, uint16_t sequence_number, int n, int k_total,
                                    int k_i);
    static bool parse_header(const uint8_t* data, size_t len, shard_header* out);

    using clock_fn = std::function<std::chrono::steady_clock::time_point()>;
    /* Replaces steady_clock::now() (tests). */
    void set_clock(clock_fn fn)
    {
        clock = std::move(fn);
    }

    rs_block_erasure();

    /*
     * TX: k data shards per block, n - k parity shards (n == k: data only, no parity).
     * max_datagram bounds the whole wire datagram. init() and disable() keep the data sequence
     * counter running so a runtime k/n change does not replay ids the receiver already saw.
     */
    bool init(int k, int n, size_t max_datagram);
    /* Stop TX encode; flush() the open block first to keep its parity. */
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
    /* Largest app datagram push_app() takes; the same for every k/n. */
    [[nodiscard]] size_t max_original() const
    {
        return max_app_payload(max_datagram_);
    }
    [[nodiscard]] static size_t max_app_payload(size_t max_datagram);
    /* Largest data payload a receiver must accept for this datagram limit. */
    [[nodiscard]] static size_t max_data_payload(size_t max_datagram);

    [[nodiscard]] uint16_t tx_sequence() const
    {
        return tx_seq;
    }
    // Overrides the random start of the TX sequence; tests use it to cross the u16 wrap.
    void set_tx_sequence(uint16_t seq)
    {
        tx_seq = seq;
    }

    /*
     * Shard buffers come from alloc(capacity) and must be at least that large; without one they
     * are heap allocated. Each output buffer has headroom bytes free in front of its header for
     * the caller, and size() covers headroom + shard. Data and parity bodies start 16-byte
     * aligned, which the PIE kernel needs.
     */
    using shard_alloc = std::function<bfc::shared_sized_buffer(size_t capacity)>;
    void set_tx_buffers(size_t headroom, shard_alloc alloc);

    struct tx_out
    {
        /* The data shard (if any) first, then the closed block's parity, row order. */
        std::vector<bfc::shared_sized_buffer> shards;
        /* Parity shards at the end of shards (0 while the block is open). */
        int parity = 0;
        /* Data shards of the closed block, and when its first one was pushed. */
        int                                   block_k = 0;
        std::chrono::steady_clock::time_point block_start {};
    };
    /*
     * App datagram -> its data shard, plus the block's parity when it closes the block. The
     * encoder reads a data shard's buffer again (its body and the zeroed capacity behind it) when
     * the block closes, so whoever takes a shard may write only into its headroom until then.
     */
    void push_app(const uint8_t* data, size_t len, tx_out* out);
    /* Closes the open block early (reconfigure / stop): its parity covers the data so far. */
    void flush(tx_out* out);

    // RX: data shards in sequence go out at once; poll_rx() releases what deadlines free.
    void push_air(bfc::shared_sized_buffer shard, fec_rx_payload_list* out);
    void push_air(const uint8_t* data, size_t len, fec_rx_payload_list* out);
    void poll_rx(fec_rx_payload_list* out);
    [[nodiscard]] bool have_spread_estimate() const
    {
        return fitted_blocks >= 2;
    }
    /* Estimated sender spread and arrival jitter (ms); the cap / 0 before. */
    [[nodiscard]] double spread_estimate_ms() const;
    [[nodiscard]] double jitter_estimate_ms() const;

    uint64_t recovered() const
    {
        return recovered_count;
    }
    uint64_t blocks() const
    {
        return blocks_count;
    }
    /* Blocks that lost data: given up, failed to decode or evicted. */
    uint64_t decode_fail() const
    {
        return given_up_blocks_count + rs_failures_count + evicted_blocks_count;
    }
    uint64_t hdr_errors() const
    {
        return hdr_errors_count;
    }
    /* Parity whose k/n disagrees with earlier parity of the same block. */
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
    /* Data shards given up plus parity never seen of blocks that are done. */
    uint64_t missing_shards() const
    {
        return missing_data_count + missing_parity_count;
    }
    /* Data shards behind the next id to release that were never released here. */
    uint64_t late_blocks() const
    {
        return late_count;
    }
    uint64_t new_references() const
    {
        return new_references_count;
    }
    uint64_t given_up_blocks() const
    {
        return given_up_blocks_count;
    }
    uint64_t encode_kernel_us() const
    {
        return encode_kernel_us_;
    }
    uint64_t oversized() const
    {
        return oversized_count;
    }
    // Interval since last take. Lifetime totals stay in the accessors above.
    uint64_t take_recovered();
    uint64_t take_decode_fail();
    uint64_t take_fail_missing_shards();
    /* Data shards given up (app datagrams lost after FEC). */
    uint64_t take_fail_lost_app_pkts();
    /* Parity shards never seen, counted when their block is done (wire loss). */
    uint64_t take_missing_parity();

    /*
     * Decodes one block. rows: k_total data rows (empty = missing; else the data payload) and
     * n parity rows (empty = missing; else the parity body after its header). Fills the missing
     * data rows with their payloads. False if fewer than k_total rows are present.
     */
    static bool decode_rows(int k_total, int n, std::vector<bfc::shared_sized_buffer>* data,
                            const std::vector<bfc::shared_sized_buffer>& parity, int* recovered);

private:
    struct rx_slot
    {
        bfc::shared_sized_buffer payload; /* the app datagram; empty = none */
        uint16_t                 seq = 0;
        /* Received (not recovered): its arrival dates its block's close. */
        bool                                  received = false;
        std::chrono::steady_clock::time_point at {};
    };
    struct rx_block
    {
        int                                   k_total = 0;
        int                                   n = 0;
        std::vector<bfc::shared_sized_buffer> parity; /* bodies by row; empty = not received */
        int                                   parity_have = 0;
        /* (row, arrival) of the parity received, for the span fit. */
        std::vector<std::pair<int, std::chrono::steady_clock::time_point>> arrivals;
        std::chrono::steady_clock::time_point                              last_seen {};
        /* Decoded, complete without decode, or failed: nothing more to recover. */
        bool done = false;
        /* Counted and learnt from; kept only so late parity of it is dropped. */
        bool finished = false;
    };

    std::chrono::steady_clock::time_point now() const;
    static int seq_dist(uint16_t a, uint16_t b);
    rx_slot*   slot_for(uint16_t seq);
    void       new_reference(uint16_t seq);
    void       push_data(const shard_header& h, bfc::shared_sized_buffer&& shard,
                         fec_rx_payload_list* out);
    void       push_parity(const shard_header& h, bfc::shared_sized_buffer&& shard,
                           fec_rx_payload_list* out);
    void       try_decode(uint16_t base, rx_block* block);
    void       run_emit_engine(fec_rx_payload_list* out);
    /* The block that holds seq, from parity seen or the last known block layout. */
    bool       block_of(uint16_t seq, uint16_t* base, int* k_total, int* n) const;
    bool       close_time(uint16_t base, int k_total,
                          std::chrono::steady_clock::time_point* at) const;
    bool       first_time(uint16_t base, int k_total,
                          std::chrono::steady_clock::time_point* at) const;
    /* When a hole in the block is given up; false if the block's close is not known yet. */
    bool       hole_deadline(uint16_t base, int k_total, int n,
                             std::chrono::steady_clock::time_point* at) const;
    std::chrono::nanoseconds margin_now() const;
    void       learn_block(uint16_t base, const rx_block& block);
    void       prune_blocks();
    void       finish_block(uint16_t base, rx_block* block);

    struct tx_row
    {
        bfc::shared_sized_buffer buf;
        size_t                   start = 0; /* where the output view starts */
        size_t                   len = 0;   /* data payload length */
    };
    [[nodiscard]] bool alloc_tx_row(tx_row* row, size_t header_len, size_t body_max);
    bool               encode_block(tx_out* out);

    bool                 active = false;
    int                  cfg_k = 0;
    int                  cfg_n = 0;
    int                  p = 0;
    size_t               max_datagram_ = 0;
    /* Encode tables of a full block (k = cfg_k); short blocks look theirs up. */
    const uint8_t*       full_g_tbls = nullptr;

    std::vector<tx_row>                   pending_rows;
    uint16_t                              block_base = 0;
    std::chrono::steady_clock::time_point block_start_ {};
    size_t                                tx_headroom = 0;
    shard_alloc                           tx_alloc;
    uint16_t                              tx_seq = 0;

    std::vector<rx_slot>         ring;
    std::map<uint16_t, rx_block> rx_blocks; /* keyed by base; ordered via seq_dist when needed */
    bool                         emit_base_set = false;
    uint16_t                     emit_next = 0;
    uint16_t                     highest_seen = 0;
    /* Newest block seen in parity, for holes whose block has no parity yet. Blocks after it are
     * taken to be layout_step_k long: the larger k of it and the block before, so a short
     * flushed block does not shorten the blocks that follow. */
    bool     layout_known = false;
    uint16_t layout_base = 0;
    int      layout_k = 0;
    int      layout_n = 0;
    int      layout_step_k = 0;
    int      data_since_parity = 0;
    bool     parity_seen = false;
    /* When the current run of holes was first seen (bounds the wait when the close of their
     * block is unknown). */
    bool                                  hole_timed = false;
    std::chrono::steady_clock::time_point hole_since {};
    /* Last data shard given up and its block (given_up_blocks counts each block once; a run of
     * holes with no known block counts once). */
    bool     gave_up_any = false;
    bool     gave_up_known = false;
    uint16_t gave_up_base = 0;
    uint16_t gave_up_last = 0;

    clock_fn clock;
    /* Running sender spread, last-parity bias and jitter estimates, seconds. */
    double spread_est_s = 0.;
    double bias_est_s = 0.;
    double jitter_est_s = 0.;
    int    spread_blocks = 0;
    int    fitted_blocks = 0;

    uint64_t         recovered_count = 0;
    uint64_t         recovered_seen = 0;
    uint64_t         blocks_count = 0;
    uint64_t         decode_fail_seen = 0;
    uint64_t         hdr_errors_count = 0;
    uint64_t         kn_mismatch_count = 0;
    uint64_t         evicted_blocks_count = 0;
    uint64_t         rs_failures_count = 0;
    uint64_t         missing_data_count = 0;
    uint64_t         missing_parity_count = 0;
    uint64_t         missing_shards_seen = 0;
    uint64_t         lost_app_seen = 0;
    uint64_t         missing_parity_seen = 0;
    uint64_t         late_count = 0;
    uint64_t         given_up_blocks_count = 0;
    uint64_t         new_references_count = 0;
    uint64_t         oversized_count = 0;
    mutable uint64_t encode_kernel_us_ = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_RS_BLOCK_ERASURE_HPP
