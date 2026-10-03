#ifndef VSTREAMER_APPS_RX_RX_STATE_HPP
#define VSTREAMER_APPS_RX_RX_STATE_HPP

#include <atomic>

namespace vstreamer::apps::rx
{

struct rx_state
{
    std::atomic<bool> skip_decode {false};
    std::atomic<bool> dec_opened {false};
};

}  // namespace vstreamer::apps::rx

#endif
