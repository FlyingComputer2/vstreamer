#ifndef VSTREAMER_CORE_PIPELINE_QUEUE_HPP
#define VSTREAMER_CORE_PIPELINE_QUEUE_HPP

#include <cstddef>
#include <cstdint>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

#include "core/component_pdu.hpp"
#include "core/pdu_wakeup.hpp"

namespace vstreamer
{

class component_sink;
class component_source;

/*
 * Bounded PDU channel between two pipelines (the only thread crossing in the pipeline model).
 *
 * One pipeline writes through a queue sink node (pipeline::add_queue_sink), another reads
 * through a queue source node (pipeline::add_queue_source). Shared by shared_ptr, so either
 * pipeline may be destroyed first.
 *
 * When full, push() drops the oldest data PDU so a slow consumer never stalls the producer
 * pipeline (a stale frame is worth less than a fresh one). Caps PDUs are never dropped: a
 * consumer that missed a caps change would misread every frame after it.
 *
 * push() notifies the consumer pipeline's wakeup, so the reading pipeline sleeps until data
 * arrives instead of polling.
 *
 * Query keys on both endpoint nodes:
 *   depth      PDUs queued now
 *   capacity   configured capacity
 *   pushed     total PDUs accepted
 *   drops      total PDUs dropped because the queue was full
 * Configure keys:
 *   capacity   1..1024, live (shrinking drops the oldest data PDUs)
 */
class pipeline_queue
{
public:
    explicit pipeline_queue(size_t capacity);

    pipeline_queue(const pipeline_queue &) = delete;
    pipeline_queue &operator=(const pipeline_queue &) = delete;

    /* Producer side. 0, or -ECANCELED after close(). */
    int push(component_pdu &&pdu);
    /* Consumer side. 0, -EAGAIN when empty, -ECANCELED when closed and empty (the queue source
     * node reports that as -EAGAIN: a stopped producer is not an error for the consumer). */
    int pop(component_pdu &out);

    /* Producer pipeline stopped; the consumer drains what is left, then sees -ECANCELED.
     * reopen() undoes it when the producer starts again and drops anything still queued from
     * the previous session. */
    void close();
    void reopen();

    /* Set by the consumer pipeline when its queue source node joins the graph. */
    void bind_consumer_wakeup(std::shared_ptr<pdu_wakeup> w);

    [[nodiscard]] size_t   depth() const;
    [[nodiscard]] size_t   capacity() const;
    [[nodiscard]] uint64_t pushed() const;
    [[nodiscard]] uint64_t drops() const;
    int                    set_capacity(size_t capacity);

private:
    void trim_locked(size_t limit);

    mutable std::mutex          mu_;
    std::deque<component_pdu>   q_;
    size_t                      capacity_;
    bool                        closed_ = false;
    std::shared_ptr<pdu_wakeup> consumer_wake_;
    std::atomic<uint64_t>       pushed_ {0};
    std::atomic<uint64_t>       drops_ {0};
};

/* Endpoint nodes; pipeline::add_queue_sink / add_queue_source build these. The sink closes the
 * queue on close() and reopens it on open(). */
[[nodiscard]] std::unique_ptr<component_sink>   make_pipeline_queue_sink(std::shared_ptr<pipeline_queue> q);
[[nodiscard]] std::unique_ptr<component_source> make_pipeline_queue_source(std::shared_ptr<pipeline_queue> q);

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PIPELINE_QUEUE_HPP
