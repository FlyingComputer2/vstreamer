#ifndef VSTREAMER_TEST_APP_LINK_EMULATOR_HPP
#define VSTREAMER_TEST_APP_LINK_EMULATOR_HPP

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

#include "test_app/stream_sdl/channel_ports.hpp"

namespace vstreamer::test_app
{

/*
 * Bidirectional UDP relay between two endpoints (e.g. stream_sender ↔ stream_receiver).
 * Applies max throughput (drop) then constant random loss on each direction independently.
 */
class link_emulator
{
public:
    link_emulator();
    ~link_emulator();

    link_emulator(const link_emulator &) = delete;
    link_emulator &operator=(const link_emulator &) = delete;

    void set_bind_host(const char *host);

    int start(int ingress_port = k_chan_fwd_ingress, const char *egress_host = k_loopback_host,
              int egress_port = k_stream_rx_listen,
              int reverse_ingress_port = k_chan_rev_ingress,
              const char *reverse_egress_host = k_loopback_host,
              int reverse_egress_port = k_chan_rev_egress);
    void stop();

    void set_max_kbps(double kbps);
    void set_drop_dt_ms(int ms);
    void set_constant_loss(double pct);
    void set_queue_depth(int depth);

    [[nodiscard]] double max_kbps() const;
    [[nodiscard]] int drop_dt_ms() const;
    [[nodiscard]] double constant_loss() const;
    [[nodiscard]] int queue_depth() const;
    [[nodiscard]] size_t forward_queue_size() const;

    struct forward_stats
    {
        uint64_t pkts_in = 0;
        uint64_t pkts_out = 0;
        uint64_t bytes_in = 0;
        uint64_t bytes_out = 0;
        uint64_t dropped_rate = 0;
        uint64_t dropped_loss = 0;
        uint64_t dropped_queue = 0;
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

        uint32_t rng = 1;

        std::atomic<uint64_t> pkts_in {0};
        std::atomic<uint64_t> pkts_out {0};
        std::atomic<uint64_t> bytes_in {0};
        std::atomic<uint64_t> bytes_out {0};
        std::atomic<uint64_t> dropped_rate {0};
        std::atomic<uint64_t> dropped_loss {0};
        std::atomic<uint64_t> dropped_queue {0};

        std::deque<std::vector<uint8_t>> ingress_queue;
        std::atomic<size_t>              ingress_queue_len {0};
    };

    int setup_direction(direction_state &dir, int ingress_port, const char *egress_host,
                        int egress_port);
    void teardown_direction(direction_state &dir);
    void relay_thread_main();
    void stop_relay();
    bool should_drop_rate(direction_state &dir, size_t pkt_bytes);
    bool should_drop_loss(direction_state &dir);
    void accept_ingress(direction_state &dir, const uint8_t *buf, size_t n);
    enum class egress_status
    {
        ok,
        rate_limited,
        loss_dropped,
        no_route,
        send_failed,
    };
    egress_status try_egress_one(direction_state &dir, const uint8_t *buf, size_t n);
    void egress_packet(direction_state &dir, const uint8_t *buf, size_t n);
    void flush_ingress_queue(direction_state &dir);
    void reset_rate_windows(double t);

    std::atomic<bool> relay_stop {false};

    direction_state fwd;
    direction_state rev;
    bool            rev_enabled = false;

    std::thread relay_thread;

    std::string bind_host = k_loopback_host;

    mutable std::mutex cfg_mu;
    double max_kbps_limit = 0.;
    int    rate_drop_dt_ms = k_chan_default_drop_dt_ms;
    double loss_pct = 0.;
    size_t ingress_queue_depth = static_cast<size_t>(k_chan_default_queue_depth);
};

}  // namespace vstreamer::test_app

#endif  // VSTREAMER_TEST_APP_LINK_EMULATOR_HPP
