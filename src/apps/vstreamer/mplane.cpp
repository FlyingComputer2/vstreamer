#include "apps/vstreamer/mplane.hpp"

#include <string_view>
#include <vector>

#include "apps/vstreamer/pipeline_set.hpp"
#include "core/key_util.hpp"

namespace vstreamer::app
{
namespace
{

std::string err_reply(int err)
{
    return "err " + std::to_string(err) + " " + errno_name(err) + "\n";
}

std::string result_reply(int r)
{
    return (r < 0) ? err_reply(r) : std::string("ok\n");
}

/* Splits off the next space-separated word; *rest keeps what follows, leading spaces dropped. */
std::string_view next_word(std::string_view *rest)
{
    std::string_view s = *rest;
    while (!s.empty() && ' ' == s.front())
    {
        s.remove_prefix(1);
    }
    const size_t end = s.find(' ');
    const std::string_view word = s.substr(0, end);
    s = (std::string_view::npos == end) ? std::string_view() : s.substr(end);
    while (!s.empty() && ' ' == s.front())
    {
        s.remove_prefix(1);
    }
    *rest = s;
    return word;
}

}  // namespace

std::string mplane_help()
{
    return "get <node> <key>\n"
           "set <node> <key> <value>\n"
           "nodes\n"
           "state\n"
           "start|pause|stop [<pipeline>|all]";
}

bool handle_mplane_line(pipeline_set &set, const char *line, std::string &reply)
{
    std::string_view       rest(line);
    const std::string_view verb = next_word(&rest);

    if ("get" == verb)
    {
        if (rest.empty())
        {
            return false; /* bare "get" is app_console's metrics report */
        }
        const std::string_view node = next_word(&rest);
        const std::string_view key = next_word(&rest);
        if (key.empty() || !rest.empty())
        {
            reply = "err usage: get <node> <key>\n";
            return true;
        }
        std::string value;
        const int   r = set.query(node, key, &value);
        reply = (r < 0) ? err_reply(r) : "ok " + value + "\n";
        return true;
    }
    if ("set" == verb)
    {
        const std::string_view node = next_word(&rest);
        const std::string_view key = next_word(&rest);
        if (key.empty() || rest.empty())
        {
            reply = "err usage: set <node> <key> <value>\n";
            return true;
        }
        reply = result_reply(set.configure(node, key, rest));
        return true;
    }
    if ("nodes" == verb)
    {
        reply.clear();
        for (const auto &n : set.nodes())
        {
            reply += "ok " + n.pipeline + " " + n.name + " " + n.type + "\n";
        }
        return true;
    }
    if ("state" == verb)
    {
        reply.clear();
        for (const auto &[name, state] : set.states())
        {
            reply += "ok " + name + " " + pipeline_state_name(state) + "\n";
        }
        return true;
    }
    if ("start" == verb || "pause" == verb || "stop" == verb)
    {
        const std::string_view target = next_word(&rest);
        if (!rest.empty())
        {
            reply = "err usage: " + std::string(verb) + " [<pipeline>|all]\n";
            return true;
        }
        int r = 0;
        if ("start" == verb)
        {
            r = set.start(target);
        }
        else if ("pause" == verb)
        {
            r = set.pause(target);
        }
        else
        {
            r = set.stop(target);
        }
        reply = result_reply(r);
        return true;
    }
    return false;
}

}  // namespace vstreamer::app
