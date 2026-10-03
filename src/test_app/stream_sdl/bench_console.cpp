#include "test_app/stream_sdl/bench_console.hpp"

#include "apps/common/tx/tx_console.hpp"
#include "test_app/stream_sdl/link_emulator.hpp"

#include "components/stream_sender.hpp"
#include "core/component_coder.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace vstreamer::test_app
{
namespace
{

bool parse_long(const char *arg, long *out)
{
    char *end = nullptr;
    const long v = std::strtol(arg, &end, 10);
    if (end == arg)
    {
        return false;
    }
    *out = v;
    return true;
}

}  // namespace

bench_console::bench_console() = default;

bench_console::~bench_console()
{
    stop();
}

void bench_console::set_bind_host(const char *host)
{
    inner_.set_bind_host(host);
}

void bench_console::set_stream_sender(vstreamer::stream_sender *sender)
{
    stream_tx = sender;
}

void bench_console::set_pipeline_metrics(const vstreamer::metrics *source)
{
    inner_.set_pipeline_metrics(source);
}

void bench_console::set_pipeline_metrics_refresh(std::function<void()> refresh)
{
    inner_.set_source_state_metrics_refresh(std::move(refresh));
}

void bench_console::set_pipeline_metrics_sync_live(std::function<void()> sync_live)
{
    inner_.set_pipeline_metrics_sync_live(std::move(sync_live));
}

void bench_console::set_source_state_metrics_refresh(std::function<void()> refresh)
{
    inner_.set_source_state_metrics_refresh(std::move(refresh));
}

void bench_console::set_encode_target(vstreamer::component_coder *encoder)
{
    encode_target = encoder;
}

void bench_console::set_encode_command_handlers(std::function<bool(int kbps)> set_cbr_kbps,
                                                std::function<bool(int qp)> set_qp,
                                                std::function<bool(int gop)> set_gop,
                                                std::function<bool()> force_idr)
{
    encode_set_cbr_kbps = std::move(set_cbr_kbps);
    encode_set_qp = std::move(set_qp);
    encode_set_gop = std::move(set_gop);
    encode_force_idr = std::move(force_idr);
}

void bench_console::register_handlers()
{
    if (handlers_registered)
    {
        return;
    }
    handlers_registered = true;

    const char *link_help =
        "set_max_kbps <kbps>\n"
        "set_drop_dt_ms <ms>\n"
        "set_constant_loss <pct>\n"
        "stats\n";

    inner_.add_handler(
        [this](const char *work, std::string &reply) -> bool {
            if (0 == std::strncmp(work, "set_max_kbps ", 13))
            {
                if (nullptr == link)
                {
                    reply = "err link not configured\n";
                    return true;
                }
                char       *end = nullptr;
                const double v = std::strtod(work + 13, &end);
                if (end == work + 13)
                {
                    reply = "err bad value\n";
                    return true;
                }
                link->set_max_kbps(v);
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_drop_dt_ms ", 15))
            {
                if (nullptr == link)
                {
                    reply = "err link not configured\n";
                    return true;
                }
                long v = 0;
                if (!parse_long(work + 15, &v) || v < k_chan_min_drop_dt_ms ||
                    v > k_chan_max_drop_dt_ms)
                {
                    reply = "err bad value\n";
                    return true;
                }
                link->set_drop_dt_ms(static_cast<int>(v));
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_constant_loss ", 18))
            {
                if (nullptr == link)
                {
                    reply = "err link not configured\n";
                    return true;
                }
                char       *end = nullptr;
                const double v = std::strtod(work + 18, &end);
                if (end == work + 18)
                {
                    reply = "err bad value\n";
                    return true;
                }
                link->set_constant_loss(v);
                reply = "ok\n";
                return true;
            }
            if (0 == std::strcmp(work, "stats"))
            {
                if (nullptr == link)
                {
                    reply = "err link not configured\n";
                    return true;
                }
                const auto fwd = link->forward_stats_snapshot();
                const auto rev = link->reverse_stats_snapshot();
                char       msg[512];
                std::snprintf(msg, sizeof(msg),
                              "fwd in=%" PRIu64 " out=%" PRIu64 " drop_rate=%" PRIu64
                              " drop_loss=%" PRIu64 " drop_queue=%" PRIu64
                              " | rev in=%" PRIu64 " out=%" PRIu64 " drop_rate=%" PRIu64
                              " drop_loss=%" PRIu64 " drop_queue=%" PRIu64
                              " | max_kbps=%.0f drop_dt_ms=%d queue=%d loss_pct=%.2f\n",
                              fwd.pkts_in, fwd.pkts_out, fwd.dropped_rate, fwd.dropped_loss,
                              fwd.dropped_queue, rev.pkts_in, rev.pkts_out, rev.dropped_rate,
                              rev.dropped_loss, rev.dropped_queue, link->max_kbps(),
                              link->drop_dt_ms(), link->queue_depth(), link->constant_loss());
                reply = msg;
                return true;
            }
            return false;
        },
        link_help);

    apps::tx::tx_console_targets tx_targets;
    tx_targets.sender = stream_tx;
    tx_targets.encoder = encode_target;
    tx_targets.set_cbr_kbps = encode_set_cbr_kbps;
    tx_targets.set_qp = encode_set_qp;
    tx_targets.set_gop = encode_set_gop;
    tx_targets.force_idr = encode_force_idr;
    apps::tx::register_tx_console_handlers(inner_, tx_targets);
}

int bench_console::start(link_emulator &link_em, int console_port)
{
    link = &link_em;
    register_handlers();
    const int rc = inner_.start(console_port);
    if (rc == 0)
    {
        std::fprintf(stderr, "channel_controller: console udp :%d\n", console_port);
    }
    return rc;
}

void bench_console::stop()
{
    inner_.stop();
}

}  // namespace vstreamer::test_app
