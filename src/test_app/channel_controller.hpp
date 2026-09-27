#ifndef VSTREAMER_TEST_APP_CHANNEL_CONTROLLER_HPP
#define VSTREAMER_TEST_APP_CHANNEL_CONTROLLER_HPP

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <netinet/in.h>

#include "test_app/channel_ports.hpp"

namespace vstreamer
{
class metrics;
class component_coder;
class stream_sender;
}

namespace vstreamer::test_app
{

/*
 * Bidirectional UDP relay between two endpoints (e.g. stream_sender ↔ stream_receiver).
 * Applies max throughput (drop) then constant random loss on each direction independently.
 */
class channel_controller
{
public:
    channel_controller();
    ~channel_controller();

    channel_controller(const channel_controller &) = delete;
    channel_controller &operator=(const channel_controller &) = delete;

    /*
     * Forward: bind ingress_port, send to egress_host:egress_port.
     * Reverse: bind reverse_ingress_port, send to reverse_egress_host:reverse_egress_port.
     * Pass reverse_ingress_port <= 0 to disable reverse.
     * Defaults: see channel_ports.hpp (5000→5001 fwd, 5002→5003 rev).
     */
    int start(int ingress_port = k_chan_fwd_ingress, const char *egress_host = k_loopback_host,
              int egress_port = k_stream_rx_listen,
              int reverse_ingress_port = k_chan_rev_ingress,
              const char *reverse_egress_host = k_loopback_host,
              int reverse_egress_port = k_chan_rev_egress);
    int start_console(int console_port = k_chan_console);
    void stop();

    void set_pipeline_metrics(const vstreamer::metrics *source);
    void set_pipeline_metrics_refresh(std::function<void()> refresh);
    /* UDP metrics: sync live counters from atomics, then metrics::to_string() (must stay fast). */
    void set_pipeline_metrics_sync_live(std::function<void()> sync_live);
    void set_source_state_metrics_refresh(std::function<void()> refresh);
    void set_encode_target(vstreamer::component_coder *encoder);
    /* Non-blocking: handlers queue work for the encode thread (preferred). */
    void set_encode_command_handlers(std::function<bool(int kbps)> set_cbr_kbps,
                                     std::function<bool(int qp)> set_qp,
                                     std::function<bool(int gop)> set_gop = {});
    void set_stream_sender(vstreamer::stream_sender *sender);

    void set_max_kbps(double kbps);
    void set_drop_dt_ms(int ms);
    void set_constant_loss(double pct);
    void set_queue_depth(int depth);

    [[nodiscard]] double max_kbps() const;
    [[nodiscard]] int drop_dt_ms() const;
    [[nodiscard]] double constant_loss() const;
    [[nodiscard]] int queue_depth() const;
    /* Forward-path ingress datagrams waiting to egress (relay thread only mutates). */
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

    /* Forward-path relay counters (stream_sender → stream_receiver leg). */
    [[nodiscard]] forward_stats forward_stats_snapshot() const;
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
    void console_thread_main();
    void stop_relay();
    void stop_console();
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
    void handle_console_line(const char *line, int reply_fd, const sockaddr_in &reply);
    void send_pipeline_metrics(int reply_fd, const sockaddr_in &reply);

    std::atomic<bool> relay_stop {false};
    std::atomic<bool> console_stop {false};

    direction_state fwd;
    direction_state rev;
    bool            rev_enabled = false;

    std::thread relay_thread;

    int console_fd = -1;
    std::thread console_thread;

    mutable std::mutex cfg_mu;
    double max_kbps_limit = 0.;
    int    rate_drop_dt_ms = k_chan_default_drop_dt_ms;
    double loss_pct = 0.;
    size_t ingress_queue_depth = static_cast<size_t>(k_chan_default_queue_depth);

    const vstreamer::metrics *pipeline_metrics = nullptr;
    std::function<void()> pipeline_metrics_refresh;
    std::function<void()> pipeline_metrics_sync_live;
    std::function<void()> source_state_metrics_refresh;
    vstreamer::component_coder *encode_target = nullptr;
    std::function<bool(int kbps)> encode_set_cbr_kbps;
    std::function<bool(int qp)>   encode_set_qp;
    std::function<bool(int gop)>  encode_set_gop;
    vstreamer::stream_sender     *stream_tx = nullptr;
};

}  // namespace vstreamer::test_app

#endif  // VSTREAMER_TEST_APP_CHANNEL_CONTROLLER_HPP
