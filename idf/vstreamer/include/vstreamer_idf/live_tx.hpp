#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vstreamer::idf
{

/* Component settings as (key, value) pairs, applied with configure() in order before open(). */
using kv_list = std::vector<std::pair<std::string, std::string>>;

struct live_tx_config
{
    /* stream_sender "stream" destination, host:port. */
    std::string peer;
    /* Stream this long once the first AU is out; 0 = until reset. */
    int seconds = 0;
    /* UDP port of the management plane (get/set src|pay|snd <key>, get sys stats); 0 = none. */
    int mplane_port = 5090;
    /* Seconds between "live TX ..." stats lines; 0 = none. */
    int stats_interval_s = 10;

    kv_list src {{"buffers", "6"}, {"fps", "30"},     {"bitrate", "10000000"},
                 {"gop", "30"},    {"qp_min", "20"}, {"qp_max", "51"}};
    kv_list pay {{"fps", "30"}, {"mtu", "1400"}};
    kv_list snd {{"mtu", "1400"}, {"max_kbps", "0"}, {"telemetry", "off"}, {"fec", "none"}};
};

/*
 * Live video TX on the ESP32-P4: h264_isp_src (MIPI-CSI camera, ISP, hardware H.264) ->
 * rtp_h264_pay -> stream_sender to cfg.peer, with the P4 thread layout (FEC encode on core 0,
 * send thread on core 1) and an optional UDP management plane. Blocks until cfg.seconds have
 * passed (or forever) and returns 0 when the stream ran at a usable rate, -1 on failure, and 0
 * without streaming when no camera sensor answers.
 */
int run_live_tx(const live_tx_config &cfg);

}  // namespace vstreamer::idf
