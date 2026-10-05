#include "core/metrics.hpp"
#include "core/stream_telemetry.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

void store_peer_link_metrics(vstreamer::metrics &m, const vstreamer::stream_link_counters &c,
                             double loss_udp_pct, double loss_fec_pct)
{
    vstreamer::metric_store(*m.get_metric("stream_sender.peer_udp_packet_received"),
                            c.udp_packet_received);
    vstreamer::metric_store(*m.get_metric("stream_sender.peer_fec_packet_received"),
                            c.fec_packet_received);
    vstreamer::metric_store(*m.get_metric("stream_sender.peer_udp_gap_count"), c.udp_gap_count);
    vstreamer::metric_store(*m.get_metric("stream_sender.peer_fec_gap_count"), c.fec_gap_count);
    vstreamer::metric_store(*m.get_metric("stream_sender.peer_loss_udp_pct"), loss_udp_pct);
    vstreamer::metric_store(*m.get_metric("stream_sender.peer_loss_fec_pct"), loss_fec_pct);
}

}  // namespace

TEST(PeerLinkMetricsTest, CbrControllerMetricNamesResolve)
{
    vstreamer::metrics m;
    vstreamer::stream_link_counters c;
    c.udp_packet_received = 100;
    c.fec_packet_received = 95;
    c.udp_gap_count = 2;
    c.fec_gap_count = 1;
    store_peer_link_metrics(m, c, 1.5, 0.5);

    const char *names[] = {"stream_sender.peer_udp_packet_received",
                           "stream_sender.peer_fec_packet_received",
                           "stream_sender.peer_udp_gap_count",
                           "stream_sender.peer_fec_gap_count",
                           "stream_sender.peer_loss_udp_pct",
                           "stream_sender.peer_loss_fec_pct"};
    for (const char *name : names)
    {
        std::string value;
        EXPECT_TRUE(m.format_metric(name, &value)) << name;
        EXPECT_FALSE(value.empty()) << name;
    }
}
