#include "apps/channel_sim/channel_mplane.hpp"

#include "apps/common/link_emulator.hpp"
#include "core/key_util.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace vstreamer::apps
{
namespace
{

std::string err_reply(int err)
{
    const int   e = err < 0 ? -err : err;
    const char *n = ENOTSUP == e ? "ENOTSUP" : strerrorname_np(e);
    return "err " + std::to_string(-e) + " " + (nullptr != n ? n : "E?") + "\n";
}

/* Splits off the next space-separated word; *rest keeps what follows, leading spaces dropped. */
std::string_view next_word(std::string_view *rest)
{
    std::string_view s = *rest;
    while (!s.empty() && ' ' == s.front())
    {
        s.remove_prefix(1);
    }
    const size_t           end = s.find(' ');
    const std::string_view word = s.substr(0, end);
    s = (std::string_view::npos == end) ? std::string_view() : s.substr(end);
    while (!s.empty() && ' ' == s.front())
    {
        s.remove_prefix(1);
    }
    *rest = s;
    return word;
}

int parse_pct(std::string_view v, double *out)
{
    double d = 0.;
    if (key_parse_double(v, &d) != 0 || d < 0. || d > 100.)
    {
        return -EINVAL;
    }
    *out = d;
    return 0;
}

std::string config_line(const link_emulator &link)
{
    const gilbert_elliott ge = link.gilbert_elliott_params();
    char                  buf[256];
    std::snprintf(buf, sizeof(buf),
                  "loss=%g ge_p_gb=%g ge_p_bg=%g ge_loss_good=%g ge_loss_bad=%g max_kbps=%g "
                  "drop_dt_ms=%d queue=%d",
                  link.constant_loss(), ge.p_gb, ge.p_bg, ge.loss_good, ge.loss_bad,
                  link.max_kbps(), link.drop_dt_ms(), link.queue_depth());
    return buf;
}

void append_stats(std::string *out, const char *dir, const link_emulator::forward_stats &s)
{
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%s_in=%" PRIu64 " %s_out=%" PRIu64 " %s_bytes_in=%" PRIu64
                  " %s_bytes_out=%" PRIu64 " %s_drop_rate=%" PRIu64 " %s_drop_loss=%" PRIu64
                  " %s_drop_burst=%" PRIu64 " %s_bursts=%" PRIu64 " %s_drop_queue=%" PRIu64
                  " %s_no_route=%" PRIu64,
                  dir, s.pkts_in, dir, s.pkts_out, dir, s.bytes_in, dir, s.bytes_out, dir,
                  s.dropped_rate, dir, s.dropped_loss, dir, s.dropped_burst, dir, s.bursts, dir,
                  s.dropped_queue, dir, s.dropped_no_route);
    if (!out->empty())
    {
        *out += ' ';
    }
    *out += buf;
}

int get_key(const link_emulator &link, std::string_view key, std::string *value)
{
    char buf[64];
    if ("loss" == key)
    {
        std::snprintf(buf, sizeof(buf), "%g", link.constant_loss());
    }
    else if ("ge_p_gb" == key || "ge_p_bg" == key || "ge_loss_good" == key ||
             "ge_loss_bad" == key)
    {
        const gilbert_elliott ge = link.gilbert_elliott_params();
        const double          v = "ge_p_gb" == key   ? ge.p_gb
                                  : "ge_p_bg" == key ? ge.p_bg
                                  : "ge_loss_good" == key ? ge.loss_good
                                                          : ge.loss_bad;
        std::snprintf(buf, sizeof(buf), "%g", v);
    }
    else if ("max_kbps" == key)
    {
        std::snprintf(buf, sizeof(buf), "%g", link.max_kbps());
    }
    else if ("drop_dt_ms" == key)
    {
        std::snprintf(buf, sizeof(buf), "%d", link.drop_dt_ms());
    }
    else if ("queue" == key)
    {
        std::snprintf(buf, sizeof(buf), "%d", link.queue_depth());
    }
    else if ("config" == key)
    {
        *value = config_line(link);
        return 0;
    }
    else if ("stats" == key)
    {
        value->clear();
        append_stats(value, "fwd", link.forward_stats_snapshot());
        append_stats(value, "rev", link.reverse_stats_snapshot());
        return 0;
    }
    else
    {
        return -ENOTSUP;
    }
    *value = buf;
    return 0;
}

int set_key(link_emulator &link, std::string_view key, std::string_view value)
{
    if ("loss" == key)
    {
        double v = 0.;
        if (parse_pct(value, &v) < 0)
        {
            return -EINVAL;
        }
        link.set_constant_loss(v);
        return 0;
    }
    if ("ge_p_gb" == key || "ge_p_bg" == key || "ge_loss_good" == key || "ge_loss_bad" == key)
    {
        double v = 0.;
        if (parse_pct(value, &v) < 0)
        {
            return -EINVAL;
        }
        gilbert_elliott ge = link.gilbert_elliott_params();
        ("ge_p_gb" == key       ? ge.p_gb
         : "ge_p_bg" == key     ? ge.p_bg
         : "ge_loss_good" == key ? ge.loss_good
                                 : ge.loss_bad) = v;
        link.set_gilbert_elliott(ge);
        return 0;
    }
    if ("max_kbps" == key)
    {
        double v = 0.;
        if (key_parse_double(value, &v) != 0 || v < 0.)
        {
            return -EINVAL;
        }
        link.set_max_kbps(v);
        return 0;
    }
    if ("drop_dt_ms" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) != 0 || v < k_link_min_drop_dt_ms ||
            v > k_link_max_drop_dt_ms)
        {
            return -EINVAL;
        }
        link.set_drop_dt_ms(static_cast<int>(v));
        return 0;
    }
    if ("queue" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) != 0 || v < 0 || v > 1'000'000)
        {
            return -EINVAL;
        }
        link.set_queue_depth(static_cast<int>(v));
        return 0;
    }
    if ("seed" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) != 0 || v < 0 || v > UINT32_MAX)
        {
            return -EINVAL;
        }
        link.set_seed(static_cast<uint32_t>(v));
        return 0;
    }
    if ("stats" == key)
    {
        if ("reset" != value)
        {
            return -EINVAL;
        }
        link.reset_stats();
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace

std::string channel_mplane_help()
{
    return "get link <loss|ge_p_gb|ge_p_bg|ge_loss_good|ge_loss_bad|max_kbps|drop_dt_ms|queue|"
           "config|stats>\n"
           "set link <loss|ge_p_gb|ge_p_bg|ge_loss_good|ge_loss_bad|max_kbps|drop_dt_ms|queue|"
           "seed> <value>\n"
           "set link stats reset";
}

bool handle_channel_mplane_line(link_emulator &link, const char *line, std::string &reply)
{
    std::string_view       rest(line);
    const std::string_view verb = next_word(&rest);
    if ("get" != verb && "set" != verb)
    {
        return false;
    }
    const std::string_view node = next_word(&rest);
    const std::string_view key = next_word(&rest);
    if ("link" != node)
    {
        reply = err_reply(-ENOENT);
        return true;
    }
    if (key.empty() || ("get" == verb && !rest.empty()) || ("set" == verb && rest.empty()))
    {
        reply = "err usage: " + std::string(verb) + " link <key>" +
                ("set" == verb ? " <value>" : "") + "\n";
        return true;
    }
    if ("get" == verb)
    {
        std::string value;
        const int   r = get_key(link, key, &value);
        reply = r < 0 ? err_reply(r) : "ok " + value + "\n";
        return true;
    }
    const int r = set_key(link, key, rest);
    reply = r < 0 ? err_reply(r) : std::string("ok\n");
    return true;
}

}  // namespace vstreamer::apps
