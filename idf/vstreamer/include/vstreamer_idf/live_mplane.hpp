#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/default_reactor.hpp"
#include "core/thread_spawn.hpp"

namespace vstreamer
{
class component;
}

/*
 * UDP management plane for live TX (vstreamer::idf::run_live_tx), in the grammar of the Linux
 * vstreamer app: one request per datagram, replies end in '\n', success is "ok" or "ok <value>",
 * failure "err <errno> <NAME>".
 *
 *   get <node> <key>
 *   set <node> <key> <value...>
 *   ping                         "ok pong"
 *   nodes                        "ok <node>" per node
 *
 * There is no authentication: anyone on the LAN can reconfigure the stream.
 */
class live_mplane
{
public:
    using query_fn = std::function<std::string()>;
    /* Applies the value of "set <node> <key> <value>"; 0 or -errno. */
    using set_fn = std::function<int(std::string_view value)>;

    live_mplane() = default;
    ~live_mplane();
    live_mplane(const live_mplane &) = delete;
    live_mplane &operator=(const live_mplane &) = delete;

    void add_node(std::string name, vstreamer::component *node);
    /* A read-only node: "get <name> <key>" calls fn for that key. */
    void add_query(std::string name, std::string key, query_fn fn);
    /* A read/write key not backed by a component: "get" calls get, "set" calls set. */
    void add_control(std::string name, std::string key, query_fn get, set_fn set);

    /* Starts the server thread on `thread` (name, stack, core, priority); 0 or an error from
     * socket()/bind(). Nodes must outlive stop(). */
    int start(int port, const vstreamer::thread_opts &thread);
    /* Serves on the running reactor registered as reactor_name (vstreamer::default_reactor_registry)
     * instead of a thread: requests are handled on that reactor's thread, so handlers must not
     * block. -ENOENT: no such reactor. Must not be called on that reactor's thread. */
    int start(int port, const std::string &reactor_name);
    /* Not on the reactor's thread in reactor mode. */
    void stop();

    std::string handle(std::string_view line);

private:
    void serve();
    int  open_socket(int port, bool nonblocking);
    void serve_ready();

    int               fd = -1;
    std::shared_ptr<vstreamer::default_reactor> reactor; // reactor mode
    std::atomic<bool> stopping {false};
    std::thread       thread;

    std::vector<std::pair<std::string, vstreamer::component *>> nodes;
    struct query_entry
    {
        std::string node;
        std::string key;
        query_fn    fn;
        set_fn      set; // empty: read-only
    };
    std::vector<query_entry> queries;
};
