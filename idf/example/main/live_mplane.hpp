#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vstreamer
{
class component;
}

/*
 * UDP management plane for the live TX example, in the grammar of the Linux vstreamer app: one
 * request per datagram, replies end in '\n', success is "ok" or "ok <value>", failure
 * "err <errno> <NAME>".
 *
 *   get <node> <key>
 *   set <node> <key> <value...>
 *   nodes                        "ok <node>" per node
 *
 * There is no authentication: anyone on the LAN can reconfigure the stream.
 */
class live_mplane
{
public:
    using query_fn = std::function<std::string()>;

    live_mplane() = default;
    ~live_mplane();
    live_mplane(const live_mplane &) = delete;
    live_mplane &operator=(const live_mplane &) = delete;

    void add_node(std::string name, vstreamer::component *node);
    /* A read-only node: "get <name> <key>" calls fn for that key. */
    void add_query(std::string name, std::string key, query_fn fn);

    /* Starts the server thread; 0 or an error from socket()/bind(). Nodes must outlive stop(). */
    int start(int port);
    void stop();

    std::string handle(std::string_view line);

private:
    void serve();

    int               fd = -1;
    std::atomic<bool> stopping {false};
    std::thread       thread;

    std::vector<std::pair<std::string, vstreamer::component *>> nodes;
    struct query_entry
    {
        std::string node;
        std::string key;
        query_fn    fn;
    };
    std::vector<query_entry> queries;
};
