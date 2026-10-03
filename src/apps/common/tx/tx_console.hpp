#ifndef VSTREAMER_APPS_TX_TX_CONSOLE_HPP
#define VSTREAMER_APPS_TX_TX_CONSOLE_HPP

#include "apps/common/app_console.hpp"

#include <functional>

namespace vstreamer
{
class component_coder;
class stream_sender;
}

namespace vstreamer::apps::tx
{

struct tx_console_targets
{
    vstreamer::stream_sender     *sender = nullptr;
    vstreamer::component_coder   *encoder = nullptr;
    std::function<bool(int kbps)> set_cbr_kbps;
    std::function<bool(int qp)>   set_qp;
    std::function<bool(int gop)>  set_gop;
    std::function<bool()>         force_idr;
};

void register_tx_console_handlers(app_console &console, const tx_console_targets &targets);

}  // namespace vstreamer::apps::tx

#endif
