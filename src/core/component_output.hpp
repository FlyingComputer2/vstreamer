#ifndef VSTREAMER_CORE_COMPONENT_OUTPUT_HPP
#define VSTREAMER_CORE_COMPONENT_OUTPUT_HPP

#include <climits>
#include <cstdint>
#include <memory>

#include "core/component_pdu.hpp"
#include "core/pdu_wakeup.hpp"

namespace vstreamer
{

/* Producer side of a component. Besides output(), it carries the two scheduling hooks a
 * single-thread pipeline needs from producers: a wakeup the component notifies when output()
 * has data that did not come from an input() call (a camera frame, a hardware encoder
 * finishing, a decoder thread), and the next time it wants output() polled without one. */
class component_output
{
public:
    virtual ~component_output() = default;

    virtual int output(component_pdu &out) = 0;

    /* Shared by the nodes the caller schedules together; set before open(). */
    void set_wakeup(std::shared_ptr<pdu_wakeup> w) { wakeup_ = std::move(w); }

    /* Steady-clock ns at which output() should be polled even without a notify, for
     * time-driven producers (paced sources); INT64_MAX when there is none. */
    [[nodiscard]] virtual int64_t next_deadline_ns() const { return INT64_MAX; }

protected:
    void notify_wakeup()
    {
        if (wakeup_)
        {
            wakeup_->notify();
        }
    }

private:
    std::shared_ptr<pdu_wakeup> wakeup_;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_OUTPUT_HPP
