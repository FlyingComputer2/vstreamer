#include "test_app/stream_sdl/bench_console.hpp"

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

    const char *tx_help =
        "set_fec none\n"
        "set_fec_k <k>\n"
        "set_fec_n <n>\n"
        "set_encode_cbr <kbps>\n"
        "set_encode_qp <qp>\n"
        "set_gop <gop>\n"
        "force_idr\n";

    inner_.add_handler(
        [this](const char *work, std::string &reply) -> bool {
            if (0 == std::strcmp(work, "set_fec none"))
            {
                if (nullptr == stream_tx)
                {
                    reply = "err stream_sender not configured\n";
                    return true;
                }
                static const char none_mode[] = "none";
                std::string_view val = none_mode;
                if (stream_tx->configure("fec", val) < 0)
                {
                    reply = "err set_fec none\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_fec_k ", 10))
            {
                if (nullptr == stream_tx)
                {
                    reply = "err stream_sender not configured\n";
                    return true;
                }
                long k = 0;
                if (!parse_long(work + 10, &k) || k < 1 || k > 15)
                {
                    reply = "err bad k (1..15)\n";
                    return true;
                }
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%ld", k);
                std::string_view val = buf;
                if (stream_tx->configure("fec_k", val) < 0)
                {
                    reply = "err bad k (1..15)\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_fec_n ", 10))
            {
                if (nullptr == stream_tx)
                {
                    reply = "err stream_sender not configured\n";
                    return true;
                }
                long n = 0;
                if (!parse_long(work + 10, &n) || n < 1 || n > 15)
                {
                    reply = "err bad n (k..15)\n";
                    return true;
                }
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%ld", n);
                std::string_view val = buf;
                if (stream_tx->configure("fec_n", val) < 0)
                {
                    reply = "err bad n (k..15)\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_encode_cbr ", 15))
            {
                long kbps = 0;
                if (!parse_long(work + 15, &kbps) || kbps < 100 || kbps > 200'000)
                {
                    reply = "err bad kbps (100..200000)\n";
                    return true;
                }
                bool ok = false;
                if (encode_set_cbr_kbps)
                {
                    ok = encode_set_cbr_kbps(static_cast<int>(kbps));
                }
                else if (nullptr != encode_target)
                {
                    char bps_buf[32];
                    std::snprintf(bps_buf, sizeof(bps_buf), "%ld", kbps * 1000L);
                    std::string_view val = bps_buf;
                    ok = encode_target->configure("cbr", val) == 0;
                }
                if (!ok)
                {
                    reply = encode_set_cbr_kbps || nullptr != encode_target
                                ? "err set cbr failed\n"
                                : "err encoder not configured\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_encode_qp ", 14))
            {
                long qp = 0;
                if (!parse_long(work + 14, &qp) || qp < 0 || qp > 51)
                {
                    reply = "err bad qp (0..51)\n";
                    return true;
                }
                bool ok = false;
                if (encode_set_qp)
                {
                    ok = encode_set_qp(static_cast<int>(qp));
                }
                else if (nullptr != encode_target)
                {
                    char qp_buf[16];
                    std::snprintf(qp_buf, sizeof(qp_buf), "%ld", qp);
                    std::string_view val = qp_buf;
                    ok = encode_target->configure("qp", val) == 0;
                }
                if (!ok)
                {
                    reply = encode_set_qp || nullptr != encode_target ? "err set qp failed\n"
                                                                      : "err encoder not configured\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_gop ", 8))
            {
                long gop = 0;
                if (!parse_long(work + 8, &gop) || gop < 1 || gop > 255)
                {
                    reply = "err bad gop (1..255)\n";
                    return true;
                }
                bool ok = false;
                if (encode_set_gop)
                {
                    ok = encode_set_gop(static_cast<int>(gop));
                }
                else if (nullptr != encode_target)
                {
                    char gop_buf[16];
                    std::snprintf(gop_buf, sizeof(gop_buf), "%ld", gop);
                    std::string_view val = gop_buf;
                    ok = encode_target->configure("gop", val) == 0;
                }
                if (!ok)
                {
                    reply = encode_set_gop || nullptr != encode_target ? "err set gop failed\n"
                                                                       : "err encoder not configured\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strcmp(work, "force_idr"))
            {
                bool ok = false;
                if (encode_force_idr)
                {
                    ok = encode_force_idr();
                }
                else if (nullptr != encode_target)
                {
                    ok = encode_target->configure("idr", "") == 0;
                }
                if (!ok)
                {
                    reply = encode_force_idr || nullptr != encode_target
                                ? "err force_idr failed\n"
                                : "err encoder not configured\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            return false;
        },
        tx_help);
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
