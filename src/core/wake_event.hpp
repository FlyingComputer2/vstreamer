#ifndef VSTREAMER_CORE_WAKE_EVENT_HPP
#define VSTREAMER_CORE_WAKE_EVENT_HPP

#include <cstdint>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace vstreamer
{

/* Auto-reset event: notify() from any thread wakes one wait_until(), or the next one if nobody
 * waits yet. Several notifies before a wait collapse into one. */
class wake_event
{
public:
    void notify();
    /* Returns on a notify or at deadline_mono_ns (steady clock; INT64_MAX = no deadline). */
    void wait_until(int64_t deadline_mono_ns);

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    pending_ = false;
};

/* Callback that notifies w and keeps it alive, e.g. for component_output::set_output_ready(). */
[[nodiscard]] inline std::function<void()> notify_fn(std::shared_ptr<wake_event> w)
{
    return [w = std::move(w)] { w->notify(); };
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_WAKE_EVENT_HPP
