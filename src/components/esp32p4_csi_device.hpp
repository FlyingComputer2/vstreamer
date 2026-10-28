#ifndef VSTREAMER_COMPONENTS_ESP32P4_CSI_DEVICE_HPP
#define VSTREAMER_COMPONENTS_ESP32P4_CSI_DEVICE_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/component_pdu.hpp"

namespace vstreamer::detail
{
/* Driver access only: no task, ready queue, or wakeup API. The owner serializes
 * configure/open/close and joins any dequeue caller before close(). Frame PDUs
 * retain the driver ring until their last reference returns the buffer. */
class esp32p4_csi_device
{
   public:
    esp32p4_csi_device() = default;
    ~esp32p4_csi_device();
    esp32p4_csi_device(const esp32p4_csi_device &) = delete;
    esp32p4_csi_device &operator=(const esp32p4_csi_device &) = delete;
    int open();
    void close();
    int dequeue(component_pdu &out);  // bounded driver wait, -EAGAIN on timeout
    int configure(std::string_view key, std::string_view value);
    int query(std::string_view key, std::string *value) const;
    int live_width() const
    {
        return live_w;
    }
    int live_height() const
    {
        return live_h;
    }
    int frame_rate() const
    {
        return fps;
    }

   private:
    struct ring;
    static void release_ring(std::shared_ptr<ring> r);
    int sccb_port = 0, sccb_scl = 8, sccb_sda = 7, sccb_freq = 100000;
    int reset_pin = -1, pwdn_pin = -1;
    int width = 0, height = 0, fps = 30, light_freq = 60, nbufs = 4;
    int live_w = 0, live_h = 0;
    std::string driver;
    std::shared_ptr<ring> ring_;

    /* DQBUF window stats, read and cleared by query("dq_stats"). A frame that
     * was already waiting returns at once, so `ready` counts dequeues that
     * found a finished frame queued (capture-to-stamp delay not seen by ts_us),
     * and `done` sums the frames still finished in the driver after each one. */
    mutable std::atomic<uint32_t> dq_count{0}, dq_ready{0}, dq_done_sum{0}, dq_done_max{0};
    mutable std::atomic<uint64_t> dq_wait_us_sum{0};
};
}  // namespace vstreamer::detail
#endif
