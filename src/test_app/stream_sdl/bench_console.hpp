#ifndef VSTREAMER_TEST_APP_BENCH_CONSOLE_HPP
#define VSTREAMER_TEST_APP_BENCH_CONSOLE_HPP

#include <functional>

#include "apps/common/app_console.hpp"
#include "test_app/stream_sdl/channel_ports.hpp"

namespace vstreamer
{
class metrics;
class component_coder;
class stream_sender;
}

namespace vstreamer::test_app
{

class link_emulator;

/* UDP bench console (:5090 default): channel impairments + pipeline control verbs. */
class bench_console
{
public:
    bench_console();
    ~bench_console();

    bench_console(const bench_console &) = delete;
    bench_console &operator=(const bench_console &) = delete;

    void set_bind_host(const char *host);

    int start(link_emulator &link, int console_port);
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

private:
    void register_handlers();

    apps::app_console            inner_;
    link_emulator               *link = nullptr;
    vstreamer::component_coder  *encode_target = nullptr;
    std::function<bool(int kbps)> encode_set_cbr_kbps;
    std::function<bool(int qp)>   encode_set_qp;
    std::function<bool(int gop)>  encode_set_gop;
    std::function<bool()>         encode_force_idr;
    vstreamer::stream_sender     *stream_tx = nullptr;
    bool                         handlers_registered = false;
};

}  // namespace vstreamer::test_app

#endif  // VSTREAMER_TEST_APP_BENCH_CONSOLE_HPP
