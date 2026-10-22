#ifndef VSTREAMER_CORE_STREAM_QUEUE_TRANSPORT_HPP
#define VSTREAMER_CORE_STREAM_QUEUE_TRANSPORT_HPP

#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>

namespace vstreamer
{

// Each entry is one complete wire datagram, including its stream header.
// One consumer per queue. Producers push then wake the shared CV reactor.
using stream_transport_queue =
    bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
using stream_transport_queue_registry =
    bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_STREAM_QUEUE_TRANSPORT_HPP
