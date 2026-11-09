#include "vstreamer_idf/live_mplane.hpp"

#include "core/component.hpp"
#include "core/thread_spawn.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <future>
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
    queries.push_back({std::move(name), std::move(key), std::move(fn), {}});
}

void live_mplane::add_control(std::string name, std::string key, query_fn get, set_fn set)
{
    queries.push_back({std::move(name), std::move(key), std::move(get), std::move(set)});
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
    for (const auto &q : queries)
    {
        if (q.node != node || q.key != key)
        {
            continue;
        }
        if ("get" == verb)
        {
            return "ok " + q.fn() + "\n";
        }
        if (!q.set)
        {
            return err_reply(-ENOTSUP);
        }
        const int r = q.set(rest);
        return r < 0 ? err_reply(r) : std::string("ok\n");
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

int live_mplane::open_socket(int port, bool nonblocking)
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
    if (nonblocking)
    {
        (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }
    else
    {
        /* stop() is noticed within one receive timeout. */
        timeval tv {0, 200 * 1000};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        const int err = -errno;
        close(fd);
        fd = -1;
        return err;
    }
    return 0;
}

int live_mplane::start(int port, const vstreamer::thread_opts &thread_cfg)
{
    if (const int err = open_socket(port, false); err < 0)
    {
        return err;
    }
    stopping.store(false);
    thread = vstreamer::spawn_thread(thread_cfg, [this] { serve(); });
    return 0;
}

int live_mplane::start(int port, const std::string &reactor_name)
{
    auto r = vstreamer::default_reactor_registry::get_instance().get(reactor_name);
    if (!r)
    {
        return -ENOENT;
    }
    if (const int err = open_socket(port, true); err < 0)
    {
        return err;
    }
    reactor = std::move(r);
    /* Registration on the reactor's thread: bfc's poll reactor is not thread-safe for adds. */
    auto added = std::make_shared<std::promise<bool>>();
    auto is_added = added->get_future();
    reactor->wake_up([this, added] {
        added->set_value(reactor->add_read_rdy(fd, [this] { serve_ready(); }));
    });
    if (!is_added.get())
    {
        close(fd);
        fd = -1;
        reactor.reset();
        return -EIO;
    }
    return 0;
}

void live_mplane::stop()
{
    stopping.store(true);
    if (thread.joinable())
    {
        thread.join();
    }
    if (reactor && fd >= 0)
    {
        /* Off the poll set on the reactor's thread; after this no callback runs. */
        auto removed = std::make_shared<std::promise<void>>();
        auto is_removed = removed->get_future();
        reactor->wake_up([this, removed] {
            reactor->rem_read_rdy(fd);
            removed->set_value();
        });
        is_removed.wait();
    }
    reactor.reset();
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

void live_mplane::serve_ready()
{
    char buf[512];
    /* A bounded batch per wake, so a flood cannot hold the shared reactor. */
    for (int i = 0; i < 8; ++i)
    {
        sockaddr_in src {};
        socklen_t   len = sizeof(src);
        const int   n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&src),
                                 &len);
        if (n <= 0)
        {
            return;
        }
        const std::string reply = handle(std::string_view(buf, static_cast<size_t>(n)));
        (void)sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&src), len);
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
