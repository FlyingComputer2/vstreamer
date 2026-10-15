#ifndef VSTREAMER_APPS_VSTREAMER_PIPELINE_SET_HPP
#define VSTREAMER_APPS_VSTREAMER_PIPELINE_SET_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/pipeline.hpp"
#include "core/pipeline_queue.hpp"

namespace vstreamer
{
class component_coder;
class component_sink;
class component_source;
}  // namespace vstreamer

namespace vstreamer::app
{

/* "ENOENT" for -ENOENT (or ENOENT); "E?" when unknown. */
[[nodiscard]] const char *errno_name(int err);

/*
 * The pipelines of one vstreamer process, built from a pipeline file.
 *
 * File format: one command per line, tokens separated by spaces, '#' starts a comment.
 *
 *   queue <name> [capacity]                  cross-pipeline queue (default capacity 8)
 *   pipeline <name> [cpu=<n>]                start a pipeline; node/link lines below belong to it
 *   node <name> <factory_type> [key=value]   add a node; each key=value is a configure()
 *   link <a> -> <b> [-> <c> ...]             link nodes; <name>:<port> picks a port, @<queue> is
 *                                            the queue's writer at the end of a chain and its
 *                                            reader at the start
 *   set <node> <key> <value...>              configure(); the rest of the line is the value
 *   enable <node> [timeout_ms]               open a sink's send gate (component_sink::
 *                                            set_enabled); without it a stream_sender sends
 *                                            nothing. With a timeout the gate closes unless it
 *                                            is enabled again in time (operator deadman).
 *
 * Node names are unique across the file. A queue needs exactly one writer and one reader.
 * Queue endpoint nodes are named @<queue> in their pipelines.
 *
 * Pipelines start readers first (a pipeline that reads a queue starts before the one that
 * writes it) and stop writers first.
 */
class pipeline_set
{
public:
    /* What a factory type name created; exactly one member is set, or none when unknown. */
    struct created_node
    {
        std::unique_ptr<component_source> source;
        std::unique_ptr<component_coder>  coder;
        std::unique_ptr<component_sink>   sink;
    };
    using node_factory_fn = std::function<created_node(std::string_view type)>;

    struct node_info
    {
        std::string pipeline;
        std::string name;
        std::string type;
    };

    pipeline_set();
    ~pipeline_set();

    pipeline_set(const pipeline_set &) = delete;
    pipeline_set &operator=(const pipeline_set &) = delete;

    /* Before load. Default: component_factory (source, then coder, then sink). */
    void set_node_factory(node_factory_fn fn);

    /* Builds everything; on failure *err is "<file>:<line>: <message>" and the set is empty.
     * Only once per pipeline_set. */
    int load_file(const std::string &path, std::string *err);
    int load_text(std::string_view text, std::string_view source_name, std::string *err);

    /* Node access by global node name. -ENOENT when unknown. Any thread. */
    int configure(std::string_view node, std::string_view key, std::string_view value);
    int query(std::string_view node, std::string_view key, std::string *value) const;

    /* Sink send gate. -ENOENT unknown node, -EINVAL not a sink. Any thread. */
    int set_enabled(std::string_view node, bool on, int timeout_ms);
    int enabled(std::string_view node, bool *on) const;

    [[nodiscard]] std::vector<node_info> nodes() const;
    [[nodiscard]] std::vector<std::pair<std::string, pipeline_state_e>> states() const;

    /* Control one pipeline by name, or all ("" or "all") in start/stop order. Records the
     * wanted state, which a restart after an error restores. -ENOENT unknown pipeline. */
    int start(std::string_view target);
    int pause(std::string_view target);
    int stop(std::string_view target);

    /* Runs on the calling thread until run is false: when a pipeline reports an error, stops
     * all pipelines, waits backoff, then brings each back to its wanted state; retries while
     * starting fails. */
    void supervise(const std::atomic<bool> &run, std::chrono::milliseconds backoff);
    /* Wakes supervise() so it notices run went false. */
    void wake_supervisor();

    /* Restart attempts made by supervise(), for logs and tests. */
    [[nodiscard]] uint64_t restarts() const;

    /* Opaque; public only so the file loader in the .cpp can build into it. */
    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

}  // namespace vstreamer::app

#endif  // VSTREAMER_APPS_VSTREAMER_PIPELINE_SET_HPP
