#ifndef VSTREAMER_APPS_LINK_EMULATOR_HPP
#define VSTREAMER_APPS_LINK_EMULATOR_HPP

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

namespace vstreamer::apps
{

/* max_kbps rate-drop window (ms); budget = max_kbps * (drop_dt_ms / 1000) kilobits. */
inline constexpr int k_link_default_drop_dt_ms = 1000;
inline constexpr int k_link_min_drop_dt_ms = 1;
inline constexpr int k_link_max_drop_dt_ms = 3'600'000;

/* Per-direction ingress datagram queue (0 = forward immediately). */
inline constexpr int k_link_default_queue_depth = 32;

/*
 * Gilbert-Elliott burst loss, evaluated per datagram: the channel moves good -> bad with
 * probability p_gb and bad -> good with p_bg (both in percent), then drops the datagram with
 * loss_good or loss_bad percent for the state it is in. p_gb = 0 turns the model off. Mean burst
 * length is 100 / p_bg datagrams; the long-run bad fraction is p_gb / (p_gb + p_bg).
 */
struct gilbert_elliott
{
    double p_gb = 0.;
    double p_bg = 100.;
    double loss_good = 0.;
    double loss_bad = 100.;
};

/*
 * UDP relay between two endpoints (e.g. stream_sender → stream_receiver). The reverse direction
 * is the NAT-style return path of the forward flow: replies to the forward egress socket go back
 * from the forward ingress socket to the last forward source. Max throughput (drop), then
 * constant random loss, then Gilbert-Elliott loss apply to each direction independently.
 */
class link_emulator
{
public:
    link_emulator();
    ~link_emulator();

    link_emulator(const link_emulator &) = delete;
    link_emulator &operator=(const link_emulator &) = delete;

    void set_bind_host(const char *host);

    int start(int ingress_port, const char *egress_host, int egress_port);
    void stop();

    void set_max_kbps(double kbps);
    void set_drop_dt_ms(int ms);
    void set_constant_loss(double pct);
    void set_queue_depth(int depth);
    /* Probabilities are clamped to [0, 100]; the state of each direction restarts in good. */
    void set_gilbert_elliott(const gilbert_elliott &ge);
    /* Reseeds both directions' loss generators, for repeatable runs. */
    void set_seed(uint32_t seed);
    /* Zeroes the counters of both directions. */
    void reset_stats();

    [[nodiscard]] double max_kbps() const;
    [[nodiscard]] int drop_dt_ms() const;
    [[nodiscard]] double constant_loss() const;
    [[nodiscard]] int queue_depth() const;
    [[nodiscard]] gilbert_elliott gilbert_elliott_params() const;
    [[nodiscard]] size_t forward_queue_size() const;

    struct forward_stats
    {
        uint64_t pkts_in = 0;
        uint64_t pkts_out = 0;
        uint64_t bytes_in = 0;
        uint64_t bytes_out = 0;
        uint64_t dropped_rate = 0;
        uint64_t dropped_loss = 0;
        /* Gilbert-Elliott drops, and good -> bad transitions. */
        uint64_t dropped_burst = 0;
        uint64_t bursts = 0;
        uint64_t dropped_queue = 0;
        uint64_t dropped_no_route = 0;
    };

    [[nodiscard]] forward_stats forward_stats_snapshot() const;
    [[nodiscard]] forward_stats reverse_stats_snapshot() const;
    [[nodiscard]] const std::atomic<uint64_t> &forward_bytes_out_counter() const
    {
        return fwd.bytes_out;
    }

private:
    struct direction_state
    {
        int         ingress_fd = -1;
        int         egress_fd = -1;
        sockaddr_in egress_addr {};
        bool        have_egress = false;

        std::mutex rate_mu;
        double     rate_window_start = 0.;
        uint64_t   rate_window_bytes = 0;

        /* Loss generator and Gilbert-Elliott state, under loss_mu. */
        std::mutex loss_mu;
        uint32_t   rng = 1;
        bool       ge_bad = false;

        std::atomic<uint64_t> pkts_in {0};
        std::atomic<uint64_t> pkts_out {0};
        std::atomic<uint64_t> bytes_in {0};
        std::atomic<uint64_t> bytes_out {0};
        std::atomic<uint64_t> dropped_rate {0};
        std::atomic<uint64_t> dropped_loss {0};
        std::atomic<uint64_t> dropped_burst {0};
        std::atomic<uint64_t> bursts {0};
        std::atomic<uint64_t> dropped_queue {0};
        std::atomic<uint64_t> dropped_no_route {0};

        std::deque<std::vector<uint8_t>> ingress_queue;
        std::atomic<size_t>              ingress_queue_len {0};
    };

    enum class egress_status
    {
        ok,
        rate_limited,
        loss_dropped,
        burst_dropped,
        no_route,
        send_failed,
    };

    int setup_direction(direction_state &dir, int ingress_port, const char *egress_host,
                        int egress_port);
    void teardown_direction(direction_state &dir);
    void relay_thread_main();
    void stop_relay();
    bool should_drop_rate(direction_state &dir, size_t pkt_bytes);
    egress_status loss_verdict(direction_state &dir);
    void accept_ingress(direction_state &dir, const uint8_t *buf, size_t n);
    egress_status try_egress_one(direction_state &dir, const uint8_t *buf, size_t n);
    void count_drop(direction_state &dir, egress_status s);
    void egress_packet(direction_state &dir, const uint8_t *buf, size_t n);
    void flush_ingress_queue(direction_state &dir);
    void reset_rate_windows(double t);
    static forward_stats snapshot(const direction_state &dir);

    std::atomic<bool> relay_stop {false};

    direction_state fwd;
    /* Return path: no sockets of its own; egress_fd aliases fwd.ingress_fd and egress_addr is the
     * last forward source (relay-thread-only state). */
    direction_state rev;

    std::thread relay_thread;

    std::string bind_host = "127.0.0.1";

    mutable std::mutex cfg_mu;
    double          max_kbps_limit = 0.;
    int             rate_drop_dt_ms = k_link_default_drop_dt_ms;
    double          loss_pct = 0.;
    gilbert_elliott ge_params;
    size_t          ingress_queue_depth = static_cast<size_t>(k_link_default_queue_depth);
};

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_LINK_EMULATOR_HPP
