#ifndef VSTREAMER_APPS_CHANNEL_SIM_CHANNEL_MPLANE_HPP
#define VSTREAMER_APPS_CHANNEL_SIM_CHANNEL_MPLANE_HPP

#include <string>

namespace vstreamer::apps
{

class link_emulator;

/*
 * channel_sim management plane, in the vstreamer app's grammar: one request per UDP datagram,
 * replies end in '\n', success is "ok" or "ok <value>", failure "err <errno> <NAME>". The only
 * node is "link".
 *
 *   get link <key>
 *   set link <key> <value>
 *
 * Keys (percentages are 0..100):
 *   loss          constant random loss, %
 *   ge_p_gb       Gilbert-Elliott good -> bad probability per datagram, % (0 = off)
 *   ge_p_bg       bad -> good probability per datagram, %
 *   ge_loss_good  loss in the good state, %
 *   ge_loss_bad   loss in the bad state, %
 *   max_kbps      throughput cap, 0 = none
 *   drop_dt_ms    throughput cap window
 *   queue         ingress queue depth, datagrams (0 = forward immediately)
 *   seed          loss generator seed (set only)
 *   config        all of the above as key=value (get only)
 *   stats         counters as key=value; "set link stats reset" zeroes them
 */
[[nodiscard]] std::string channel_mplane_help();

/* Returns false when line is not an mplane verb, so app_console's built-ins (ping, help) run. */
bool handle_channel_mplane_line(link_emulator &link, const char *line, std::string &reply);

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_CHANNEL_SIM_CHANNEL_MPLANE_HPP
