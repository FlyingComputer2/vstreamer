#ifndef VSTREAMER_CORE_PIPELINE_HPP
#define VSTREAMER_CORE_PIPELINE_HPP

#include <cstdint>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vstreamer
{

class component;
class component_coder;
class component_sink;
class component_source;
class pipeline_queue;

/*
 * A graph of named nodes driven by one thread.
 *
 * Build phase (state STOPPED): add nodes, link them, configure pre-open keys. The graph is
 * fixed once the pipeline leaves STOPPED; add_* / link return -EBUSY until it is stopped again.
 *
 * Every node of one pipeline runs on the pipeline thread: open(), close(), input(), output().
 * Nodes that need their own thread affinity (sdl_sink presents on the thread that opened it)
 * therefore work without special cases. Work that should run in parallel goes in another
 * pipeline; pipelines exchange PDUs only through a pipeline_queue (see pipeline_queue.hpp).
 *
 * The pipeline thread moves PDUs along links: it pulls output() from each node until -EAGAIN and
 * pushes each PDU into the input() of every linked node. A PDU that a node refuses with -EAGAIN
 * is held on its link and retried; the upstream node is not pulled again until that link is
 * empty (backpressure, no drops inside a pipeline). When nothing moves, the thread sleeps on a
 * pdu_wakeup shared by all nodes (component::set_wakeup) until a node notifies or the earliest
 * component::next_deadline_ns() is reached.
 *
 * Caps PDUs travel in-band like any other PDU; the pipeline does not negotiate formats.
 *
 * Errors come only from input() / output() return values:
 *   -EAGAIN                      nothing ready / no room; wait
 *   input() -EINVAL / -ENOTSUP   this PDU is rejected (bad type, port, caps); dropped and
 *                                counted, pipeline keeps running
 *   output() -ENODEV             source lost its device and reconnects by itself; keep running
 *   anything else                fatal: pipeline goes to ERROR, the error handler is called
 * A component whose internal thread fails (socket gone, worker died) latches the error and
 * returns it from its next input() / output(), and calls notify_wakeup() so an idle pipeline
 * makes that call.
 *
 * Example, rover TX in two pipelines (capture on its own thread, as today):
 *
 *     auto q = std::make_shared<pipeline_queue>(8);
 *
 *     pipeline cap("capture");
 *     cap.add_source("cam", "v4l2_source");
 *     cap.add_queue_sink("to_enc", q);
 *     cap.link("cam", "to_enc");
 *
 *     pipeline enc("encode");
 *     enc.add_queue_source("from_cap", q);
 *     enc.add_coder("jdec", "jpeg_decoder");
 *     enc.add_coder("enc", "h264_encoder");
 *     enc.add_coder("pay", "rtp_h264_pay");
 *     enc.add_sink("snd", "stream_sender");
 *     enc.link_chain({"from_cap", "jdec", "enc", "pay", "snd"});
 *
 *     cap.configure("cam", "device", "/dev/video0");
 *     enc.configure("snd", "udp_transport", "192.168.1.10:5600");
 *     enc.start();
 *     cap.start();
 *     ...
 *     enc.configure("enc", "cbr", "4000000");   // any thread, while running
 *     std::string v;
 *     enc.query("snd", "max_input", &v);
 *     ...
 *     cap.stop();
 *     enc.stop();
 *
 * GS RX, with the display in its own pipeline: sdl_sink::input() presents and can block for a
 * vsync refresh, which must not hold up the receiver and decoder. Capacity 2 holds a frame
 * that arrives while the previous one waits for vsync; when full, the oldest is dropped:
 *
 *     auto shown = std::make_shared<pipeline_queue>(2);
 *
 *     pipeline rx("rx");
 *     rx.add_source("rcv", "stream_receiver");
 *     rx.add_coder("depay", "rtp_h264_depay");
 *     rx.add_coder("dec", "h264_decoder");
 *     rx.add_queue_sink("to_display", shown);
 *     rx.link_chain({"rcv", "depay", "dec", "to_display"});
 *
 *     pipeline display("display");
 *     display.add_queue_source("from_rx", shown);
 *     display.add_sink("screen", "sdl_sink");
 *     display.configure("screen", "video_driver", "kmsdrm");
 *     display.link("from_rx", "screen");
 *
 *     display.start();
 *     rx.start();
 *
 * Error handling, registered by the app that owns the pipelines. The handler runs on the
 * pipeline thread, so it must not call start()/pause()/stop() on any pipeline; it hands the
 * failure to the app thread, which decides what to do:
 *
 *     struct failure { std::string pipe, node; int err; };
 *     std::mutex mu;
 *     std::condition_variable cv;
 *     std::optional<failure> failed;
 *
 *     auto on_error = [&](pipeline &p) {
 *         p.set_error_handler([&, name = p.name()](const std::string &node, int err) {
 *             std::lock_guard<std::mutex> lock(mu);
 *             if (!failed)
 *             {
 *                 failed = failure {name, node, err};
 *             }
 *             cv.notify_one();
 *         });
 *     };
 *     on_error(cap);
 *     on_error(enc);
 *
 *     // App thread: stop both, restart after a back-off (camera unplugged, encoder reset...).
 *     while (run)
 *     {
 *         std::unique_lock<std::mutex> lock(mu);
 *         cv.wait(lock, [&] { return failed.has_value() || !run; });
 *         if (!failed)
 *         {
 *             break;
 *         }
 *         std::fprintf(stderr, "%s/%s failed (%d), restarting\n", failed->pipe.c_str(),
 *                      failed->node.c_str(), failed->err);
 *         failed.reset();
 *         lock.unlock();
 *         cap.stop();   // producer first: the queue closes, enc drains it
 *         enc.stop();
 *         std::this_thread::sleep_for(std::chrono::seconds(1));
 *         if (enc.start() < 0 || cap.start() < 0)
 *         {
 *             // open failed: pipelines are STOPPED again; retry on the next loop turn
 *             std::lock_guard<std::mutex> relock(mu);
 *             failed = failure {"app", "start", -EIO};
 *         }
 *     }
 *
 * Return codes follow the component contract: 0 on success, negative errno otherwise.
 */

enum class pipeline_state_e : uint8_t
{
    STOPPED = 0, /* nodes closed; graph editable */
    PAUSED,      /* nodes open; no PDUs move; sources keep their own buffers */
    RUNNING,     /* nodes open; PDUs move */
    ERROR,       /* a node failed; nodes stay open until stop() */
};

[[nodiscard]] const char *pipeline_state_name(pipeline_state_e s);

class pipeline
{
public:
    /* Called on the pipeline thread when a node returns an error other than -EAGAIN. The
     * pipeline is in ERROR when the callback runs; no more PDUs move until stop(). Called once
     * per entry into ERROR. Keep it short and hand the failure to an app thread: it must not
     * call start()/pause()/stop() on any pipeline (this one returns -EDEADLK; another one may be
     * blocked in its own handler and deadlock) and must not destroy this pipeline. */
    using error_fn = std::function<void(const std::string &node, int err)>;

    explicit pipeline(std::string name);
    ~pipeline(); /* stop() + join; never from the pipeline thread (error handler) */

    pipeline(const pipeline &) = delete;
    pipeline &operator=(const pipeline &) = delete;

    [[nodiscard]] const std::string &name() const;

    /* --- Build (STOPPED only; -EBUSY otherwise) --------------------------------------------
     *
     * node_name must be unique in this pipeline (-EEXIST) and non-empty (-EINVAL).
     * The by-type overloads go through component_factory; -ENODEV when the type is not built.
     */
    int add_source(std::string node_name, std::string_view factory_type);
    int add_coder(std::string node_name, std::string_view factory_type);
    int add_sink(std::string node_name, std::string_view factory_type);

    int add_source(std::string node_name, std::unique_ptr<component_source> node);
    int add_coder(std::string node_name, std::unique_ptr<component_coder> node);
    int add_sink(std::string node_name, std::unique_ptr<component_sink> node);

    /* Endpoints of a cross-pipeline queue. They are ordinary nodes: query(node, "depth"),
     * query(node, "drops") etc. work on them. */
    int add_queue_sink(std::string node_name, std::shared_ptr<pipeline_queue> q);
    int add_queue_source(std::string node_name, std::shared_ptr<pipeline_queue> q);

    /* PDUs leaving `from` on from_port go to `to` input to_port (pdu.port is rewritten).
     * One output port may feed several inputs (fan-out shares the SDU buffer).
     * -ENOENT unknown node, -EINVAL `from` has no output or `to` has no input, -EEXIST duplicate
     * link, -ELOOP the link would make a cycle. */
    int link(std::string_view from, std::string_view to);
    int link(std::string_view from, uint8_t from_port, std::string_view to, uint8_t to_port);
    /* link(a, b), link(b, c), ... on port 0. Stops at the first failure. */
    int link_chain(const std::vector<std::string_view> &nodes);

    /* --- Node access (any thread, any state) ----------------------------------------------
     *
     * Forwarded to the node's component::configure / query; the node does its own locking.
     * -ENOENT when node_name is unknown, else the component's return code (-ENOTSUP unknown
     * key, -EINVAL bad value, -EBUSY pre-open key while open).
     *
     * One key is answered by the pipeline instead of the node:
     *   pipeline.rejects   PDUs this node refused with -EINVAL / -ENOTSUP (dropped)
     */
    int configure(std::string_view node_name, std::string_view key, std::string_view value);
    int query(std::string_view node_name, std::string_view key, std::string *value) const;

    [[nodiscard]] std::vector<std::string> node_names() const;
    /* Typed access for app glue that needs component-specific calls. nullptr when unknown.
     * The pointer is valid until the pipeline is destroyed. */
    [[nodiscard]] component *node(std::string_view node_name) const;

    /* --- Control (any thread except the pipeline thread) ----------------------------------
     *
     * On ESP-IDF the caller must be a pthread (std::thread): the pipeline-thread check uses
     * std::this_thread::get_id(), and pthread_self() asserts in a plain FreeRTOS task such as
     * app_main.
     *
     * Each call is executed on the pipeline thread and returns its result. Called from the
     * pipeline thread itself (e.g. inside the error handler) they return -EDEADLK.
     *
     *   start():  STOPPED -> open all nodes (sinks first, sources last) -> RUNNING
     *             PAUSED  -> RUNNING
     *   pause():  RUNNING -> PAUSED;  STOPPED -> open all nodes -> PAUSED
     *   stop():   any     -> drop held PDUs, close all nodes (sources first) -> STOPPED
     *
     * A failing open() closes the nodes already opened, leaves the pipeline STOPPED and returns
     * that error. start()/pause() from ERROR return -EIO; call stop() first.
     */
    int start();
    int pause();
    int stop();

    [[nodiscard]] pipeline_state_e state() const;
    /* Node name and error of the failure that put the pipeline in ERROR; empty / 0 otherwise. */
    void last_error(std::string *node_name, int *err) const;

    /* STOPPED only. Pin the pipeline thread to cpu (-1 = no pinning). On Linux the pin is
     * applied after the nodes are opened, so threads that components start in open() are not
     * pinned with it; threads a component starts later (while running) do inherit it. On ESP-IDF
     * the core is fixed when the thread is created and no thread started from it inherits it. */
    int set_cpu(int cpu);
    void set_error_handler(error_fn fn);

private:
    struct impl;

    int spawn(bool run);

    std::unique_ptr<impl> impl_;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PIPELINE_HPP
