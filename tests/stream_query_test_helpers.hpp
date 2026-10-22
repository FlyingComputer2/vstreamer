#pragma once

#include <gtest/gtest.h>

#include "core/stream_metrics.hpp"
#include "core/stream_telemetry.hpp"

namespace vstreamer::test_query
{

template <typename Component>
stream_metric_tuple tuple(const Component &component, const char *key)
{
    stream_metric_tuple result;
    EXPECT_TRUE(query_stream_metric(component, key, &result)) << key;
    return result;
}

template <typename Component>
uint64_t scalar(const Component &component, const char *key)
{
    uint64_t result = 0;
    EXPECT_TRUE(query_stream_u64(component, key, &result)) << key;
    return result;
}

template <typename Receiver>
stream_link_counters receiver_counters(const Receiver &receiver)
{
    const auto udp = tuple(receiver, "udp_pkt_recv");
    const auto fec = tuple(receiver, "fec_pkt_recv");
    const auto bytes = tuple(receiver, "byt_recv");
    stream_link_counters result;
    result.udp_packet_received = udp.first;
    result.udp_gap_count = udp.second;
    result.fec_packet_received = fec.first;
    result.fec_gap_count = fec.second;
    result.udp_bytes_received = bytes.first;
    result.fec_bytes_received = bytes.second;
    return result;
}

// Tests exercise the public queries even when they need several observations.
template <typename Sender>
stream_peer_link peer(const Sender &sender)
{
    stream_peer_link result;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        const auto generation = scalar(sender, "peer_reports_received");
        const auto session = scalar(sender, "peer_session");
        stream_metric_tuple udp, fec, bytes;
        if (!query_stream_metric(sender, "peer_udp_pkt_recv", &udp))
        {
            return result;
        }
        if (!query_stream_metric(sender, "peer_fec_pkt_recv", &fec))
        {
            continue;
        }
        const bool have_bytes =
            query_stream_metric(sender, "peer_byt_recv", &bytes);
        if (session != scalar(sender, "peer_session") ||
            generation != scalar(sender, "peer_reports_received") ||
            udp.timestamp_ms != fec.timestamp_ms ||
            (have_bytes && udp.timestamp_ms != bytes.timestamp_ms))
        {
            continue;
        }
        result.have = true;
        result.report.session_id = static_cast<uint32_t>(session);
        result.report.timestamp_us = udp.timestamp_ms * 1000;
        result.report.has_byte_totals = have_bytes;
        result.report.counters.udp_packet_received = udp.first;
        result.report.counters.udp_gap_count = udp.second;
        result.report.counters.fec_packet_received = fec.first;
        result.report.counters.fec_gap_count = fec.second;
        result.report.counters.udp_bytes_received = bytes.first;
        result.report.counters.fec_bytes_received = bytes.second;
        result.reports_received = generation;
        result.reports_lost = scalar(sender, "peer_reports_lost");
        result.reports_rejected = scalar(sender, "peer_reports_rejected");
        std::string value;
        EXPECT_EQ(0, sender.query("peer_report_age_ms", &value));
        result.age_ms = std::stoll(value);
        EXPECT_EQ(0, sender.query("peer_report_interval_ms", &value));
        result.observed_interval_ms = std::stoll(value);
        return result;
    }
    ADD_FAILURE() << "peer report changed throughout sampling";
    return result;
}

}  // namespace vstreamer::test_query
