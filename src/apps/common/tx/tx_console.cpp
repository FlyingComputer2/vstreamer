#include "apps/common/tx/tx_console.hpp"

#include "components/stream_sender.hpp"
#include "core/component_coder.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace vstreamer::apps::tx
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

constexpr long k_fec_kn_min = rs_block_erasure::k_header_k_n_min;
constexpr long k_fec_kn_max = rs_block_erasure::k_header_k_n_max;

const std::string k_bad_k_reply =
    "err bad k (" + std::to_string(k_fec_kn_min) + ".." + std::to_string(k_fec_kn_max) + ")\n";
const std::string k_bad_n_reply = "err bad n (k.." + std::to_string(k_fec_kn_max) + ")\n";

}  // namespace

void register_tx_console_handlers(app_console &console, const tx_console_targets &targets)
{
    const char *tx_help =
        "set_fec none\n"
        "set_fec_k <k>\n"
        "set_fec_n <n>\n"
        "set_fec_spread <ms>\n"
        "set_fec_timeout <ms>\n"
        "set_encode_cbr <kbps>\n"
        "set_encode_qp <qp>\n"
        "set_gop <gop>\n"
        "force_idr\n";

    console.add_handler(
        [&targets](const char *work, std::string &reply) -> bool {
            vstreamer::stream_sender *const stream_tx = targets.sender;
            vstreamer::component_coder *const encode_target = targets.encoder;
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
                if (!parse_long(work + 10, &k) || k < k_fec_kn_min || k > k_fec_kn_max)
                {
                    reply = k_bad_k_reply;
                    return true;
                }
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%ld", k);
                std::string_view val = buf;
                if (stream_tx->configure("fec_k", val) < 0)
                {
                    reply = k_bad_k_reply;
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
                if (!parse_long(work + 10, &n) || n < k_fec_kn_min || n > k_fec_kn_max)
                {
                    reply = k_bad_n_reply;
                    return true;
                }
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%ld", n);
                std::string_view val = buf;
                if (stream_tx->configure("fec_n", val) < 0)
                {
                    reply = k_bad_n_reply;
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
                if (targets.set_cbr_kbps)
                {
                    ok = targets.set_cbr_kbps(static_cast<int>(kbps));
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
                    reply = targets.set_cbr_kbps || nullptr != encode_target
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
                if (targets.set_qp)
                {
                    ok = targets.set_qp(static_cast<int>(qp));
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
                    reply = targets.set_qp || nullptr != encode_target ? "err set qp failed\n"
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
                if (targets.set_gop)
                {
                    ok = targets.set_gop(static_cast<int>(gop));
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
                    reply = targets.set_gop || nullptr != encode_target ? "err set gop failed\n"
                                                                        : "err encoder not configured\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_fec_spread ", 15))
            {
                if (nullptr == stream_tx)
                {
                    reply = "err stream_sender not configured\n";
                    return true;
                }
                long ms = 0;
                char buf[16];
                if (!parse_long(work + 15, &ms) || ms < 0)
                {
                    reply = "err bad fec spread ms (>= 0)\n";
                    return true;
                }
                std::snprintf(buf, sizeof(buf), "%ld", ms);
                if (stream_tx->configure("fec_spread_ms", std::string_view(buf)) < 0)
                {
                    reply = "err bad fec spread ms (>= 0)\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strncmp(work, "set_fec_timeout ", 16))
            {
                if (nullptr == stream_tx)
                {
                    reply = "err stream_sender not configured\n";
                    return true;
                }
                long ms = 0;
                if (!parse_long(work + 16, &ms) || ms < 0 || ms > 60'000)
                {
                    reply = "err bad fec timeout ms (0..60000)\n";
                    return true;
                }
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%ld", ms);
                if (stream_tx->configure("fec_timeout_ms", std::string_view(buf)) < 0)
                {
                    reply = "err bad fec timeout ms (0..60000)\n";
                    return true;
                }
                reply = "ok\n";
                return true;
            }
            if (0 == std::strcmp(work, "force_idr"))
            {
                bool ok = false;
                if (targets.force_idr)
                {
                    ok = targets.force_idr();
                }
                else if (nullptr != encode_target)
                {
                    ok = encode_target->configure("idr", "") == 0;
                }
                if (!ok)
                {
                    reply = targets.force_idr || nullptr != encode_target
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

}  // namespace vstreamer::apps::tx
