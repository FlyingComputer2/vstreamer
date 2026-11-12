#ifndef VSTREAMER_CORE_COMPONENT_OUTPUT_HPP
#define VSTREAMER_CORE_COMPONENT_OUTPUT_HPP

#include <climits>
#include <cstdint>
#include <functional>
#include <utility>

#include "core/component_pdu.hpp"

namespace vstreamer
{

/* Producer side of a component. Besides output(), it carries the two scheduling hooks whoever
 * drives the component needs from a producer: an output-ready callback the component calls when
 * output() has data that did not come from an input() call (a camera frame, a hardware encoder
 * finishing, a decoder thread), and the next time it wants output() polled without one. How the
 * driver waits (a pipeline's wake_event, a stage loop, a reactor) is not the component's concern. */
class component_output
{
public:
    virtual ~component_output() = default;

    virtual int output(component_pdu &out) = 0;

    /* Called, from any thread, when output() may have data. Set before open(); must be cheap and
     * must not call back into the component. */
    void set_output_ready(std::function<void()> cb) { output_ready_ = std::move(cb); }

    /* Steady-clock ns at which output() should be polled even without a notify, for
     * time-driven producers (paced sources); INT64_MAX when there is none. */
    [[nodiscard]] virtual int64_t next_deadline_ns() const { return INT64_MAX; }

protected:
    void notify_output_ready() const
    {
        if (output_ready_)
        {
            output_ready_();
        }
    }

private:
    std::function<void()> output_ready_;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_OUTPUT_HPP
