#ifndef VSTREAMER_APPS_TX_TX_STATE_HPP
#define VSTREAMER_APPS_TX_TX_STATE_HPP

#include "apps/common/queues.hpp"

#include <atomic>

#include "core/component_source.hpp"

namespace vstreamer::apps::tx
{

struct tx_state
{
    std::atomic<int>  pending_console_cbr_kbps {-1};
    std::atomic<int>  pending_console_qp {-1};
    std::atomic<int>  pending_console_gop {-1};
    std::atomic<bool> pending_console_idr {false};

    vstreamer::component_source *metrics_source = nullptr;
    pipeline_queue              *metrics_nv12_q = nullptr;
};

extern tx_state g_tx;

}  // namespace vstreamer::apps::tx

#endif
