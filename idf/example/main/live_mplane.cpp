#include "live_mplane.hpp"

#include "core/component.hpp"
#include "core/thread_spawn.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

#include <sys/time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace
{

const char *errno_name(int e)
{
    switch (e < 0 ? -e : e)
    {
    case EINVAL:
        return "EINVAL";
    case ENOENT:
        return "ENOENT";
    case ENOTSUP:
        return "ENOTSUP";
    case EBUSY:
        return "EBUSY";
    case EAGAIN:
        return "EAGAIN";
    case ENOMEM:
        return "ENOMEM";
    case EBADF:
        return "EBADF";
    case ENODEV:
        return "ENODEV";
    default:
        return "E?";
    }
}

std::string err_reply(int r)
{
    const int e = r < 0 ? -r : r;
    return "err " + std::to_string(-e) + " " + errno_name(e) + "\n";
}

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

}  // namespace

void live_mplane::add_node(std::string name, vstreamer::component *node)
{
    nodes.emplace_back(std::move(name), node);
}

void live_mplane::add_query(std::string name, std::string key, query_fn fn)
{
    queries.push_back({std::move(name), std::move(key), std::move(fn)});
}

std::string live_mplane::handle(std::string_view line)
{
    while (!line.empty() && ('\n' == line.back() || '\r' == line.back() || ' ' == line.back()))
    {
        line.remove_suffix(1);
    }
    std::string_view       rest = line;
    const std::string_view verb = next_word(&rest);
    if ("nodes" == verb)
    {
        std::string reply;
        for (const auto &n : nodes)
        {
            reply += "ok " + n.first + "\n";
        }
        return reply.empty() ? std::string("ok\n") : reply;
    }
    if ("ping" == verb)
    {
        return "ok pong\n";
    }
    if ("get" != verb && "set" != verb)
    {
        return "err usage: get <node> <key> | set <node> <key> <value> | nodes\n";
    }
    const std::string_view node = next_word(&rest);
    const std::string_view key = next_word(&rest);
    if (key.empty() || ("get" == verb && !rest.empty()) || ("set" == verb && rest.empty()))
    {
        return "err usage: " + std::string(verb) + " <node> <key>" +
               ("set" == verb ? " <value>" : "") + "\n";
    }
    if ("get" == verb)
    {
        for (const auto &q : queries)
        {
            if (q.node == node && q.key == key)
            {
                return "ok " + q.fn() + "\n";
            }
        }
    }
    for (const auto &n : nodes)
    {
        if (n.first != node)
        {
            continue;
        }
        if ("get" == verb)
        {
            std::string value;
            const int   r = n.second->query(key, &value);
            return r < 0 ? err_reply(r) : "ok " + value + "\n";
        }
        const int r = n.second->configure(key, rest);
        return r < 0 ? err_reply(r) : std::string("ok\n");
    }
    return err_reply(-ENOENT);
}

live_mplane::~live_mplane()
{
    stop();
}

int live_mplane::start(int port, const vstreamer::thread_opts &thread_cfg)
{
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -errno;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    /* stop() is noticed within one receive timeout. */
    timeval tv {0, 200 * 1000};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        const int err = -errno;
        close(fd);
        fd = -1;
        return err;
    }
    stopping.store(false);
    thread = vstreamer::spawn_thread(thread_cfg, [this] { serve(); });
    return 0;
}

void live_mplane::stop()
{
    stopping.store(true);
    if (thread.joinable())
    {
        thread.join();
    }
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

void live_mplane::serve()
{
    char buf[512];
    while (!stopping.load())
    {
        sockaddr_in src {};
        socklen_t   len = sizeof(src);
        const int   n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&src),
                                 &len);
        if (n <= 0)
        {
            continue;
        }
        const std::string reply = handle(std::string_view(buf, static_cast<size_t>(n)));
        (void)sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&src), len);
    }
}
