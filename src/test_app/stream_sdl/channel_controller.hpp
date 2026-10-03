#ifndef VSTREAMER_TEST_APP_CHANNEL_CONTROLLER_HPP
#define VSTREAMER_TEST_APP_CHANNEL_CONTROLLER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "test_app/stream_sdl/bench_console.hpp"
#include "test_app/stream_sdl/link_emulator.hpp"

namespace vstreamer
{
class metrics;
class component_coder;
class stream_sender;
}

namespace vstreamer::test_app
{

/* Facade: UDP link emulator + bench console (same API as pre-P11-T5). */
class channel_controller
{
public:
    channel_controller();
    ~channel_controller();

    channel_controller(const channel_controller &) = delete;
    channel_controller &operator=(const channel_controller &) = delete;

    void set_bind_host(const char *host);

    int start(int ingress_port = k_chan_fwd_ingress, const char *egress_host = k_loopback_host,
              int egress_port = k_stream_rx_listen);
    int start_console(int console_port = k_chan_console);
    void stop();

    void set_pipeline_metrics(const vstreamer::metrics *source);
    void set_pipeline_metrics_refresh(std::function<void()> refresh);
    void set_pipeline_metrics_sync_live(std::function<void()> sync_live);
    void set_source_state_metrics_refresh(std::function<void()> refresh);
    void set_encode_target(vstreamer::component_coder *encoder);
    void set_encode_command_handlers(std::function<bool(int kbps)> set_cbr_kbps,
                                     std::function<bool(int qp)> set_qp,
                                     std::function<bool(int gop)> set_gop = {},
                                     std::function<bool()> force_idr = {});
    void set_stream_sender(vstreamer::stream_sender *sender);

    void set_max_kbps(double kbps);
    void set_drop_dt_ms(int ms);
    void set_constant_loss(double pct);
    void set_queue_depth(int depth);

    [[nodiscard]] double max_kbps() const;
    [[nodiscard]] int drop_dt_ms() const;
    [[nodiscard]] double constant_loss() const;
    [[nodiscard]] int queue_depth() const;
    [[nodiscard]] size_t forward_queue_size() const;

    using forward_stats = link_emulator::forward_stats;

    [[nodiscard]] forward_stats forward_stats_snapshot() const;
    [[nodiscard]] const std::atomic<uint64_t> &forward_bytes_out_counter() const;

private:
    link_emulator link_;
    bench_console console_;
};

}  // namespace vstreamer::test_app

#endif  // VSTREAMER_TEST_APP_CHANNEL_CONTROLLER_HPP
