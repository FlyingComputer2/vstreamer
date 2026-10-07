#ifndef VSTREAMER_CORE_PDU_WAKEUP_HPP
#define VSTREAMER_CORE_PDU_WAKEUP_HPP

#include <cstdint>

#include <condition_variable>
#include <mutex>

namespace vstreamer
{

class pdu_wakeup
{
public:
    void notify();
    void wait_until(int64_t deadline_mono_ns);

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    pending_ = false;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PDU_WAKEUP_HPP
