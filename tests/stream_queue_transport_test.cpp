#include "core/stream_queue_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <future>
#include <iterator>
#include <optional>
#include <thread>

#include "components/stream_sender.hpp"
#include "core/default_reactor_host.hpp"
#include "core/stream_header.hpp"
#include "core/stream_telemetry.hpp"
#include "stream_query_test_helpers.hpp"
#include "test_pdu_helpers.hpp"
#if defined(VSTREAMER_APP_TX_OK)
#include "apps/common/pipeline_state.hpp"
#include "apps/common/tx/tx_metrics.hpp"
#endif

namespace
{
using namespace vstreamer;

/* A data shard's sequence and payload; false for parity or a bad header. */
bool data_payload(const bfc::shared_sized_buffer &wire, uint16_t *seq, const uint8_t **payload,
                  size_t *len)
{
    rs_block_erasure::shard_header h;
    if (!rs_block_erasure::parse_header(wire.u8(), wire.size(), &h) || !h.is_data)
    {
        return false;
    }
    *seq = h.sequence_number;
    *payload = wire.u8() + h.header_len;
    *len = wire.size() - h.header_len;
    return true;
}

class QueueTransportTest : public ::testing::Test
{
   protected:
    std::shared_ptr<stream_transport_queue> tx =
        std::make_shared<stream_transport_queue>();
    std::shared_ptr<stream_transport_queue> rx =
        std::make_shared<stream_transport_queue>();
    std::shared_ptr<bfc::cv_reactor<>> reactor =
        std::make_shared<bfc::cv_reactor<>>();
    std::thread worker;

    void SetUp() override
    {
        tx->set_capacity(64);
        rx->set_capacity(64);
        ASSERT_TRUE(stream_transport_queue_registry::get_instance().add(
            "transport-test-tx", tx));
        ASSERT_TRUE(stream_transport_queue_registry::get_instance().add(
            "transport-test-rx", rx));
        ASSERT_TRUE(bfc::cv_reactor_registry::get_instance().add(
            "transport-test-reactor", reactor));
        auto started = std::make_shared<std::promise<void>>();
        auto ready = started->get_future();
        reactor->wake_up([started] { started->set_value(); });
        worker = std::thread([this] { reactor->run(); });
        ASSERT_EQ(std::future_status::ready,
                  ready.wait_for(std::chrono::seconds(2)));
    }

    void TearDown() override
    {
        reactor->stop();
        if (worker.joinable())
            worker.join();
        bfc::cv_reactor_registry::get_instance().rem("transport-test-reactor");
        stream_transport_queue_registry::get_instance().rem(
            "transport-test-tx");
        stream_transport_queue_registry::get_instance().rem(
            "transport-test-rx");
    }

    void configure(stream_sender &sender)
    {
        ASSERT_EQ(0,
                  sender.configure("queue_transport_tx", "transport-test-tx"));
        ASSERT_EQ(0,
                  sender.configure("queue_transport_rx", "transport-test-rx"));
        ASSERT_EQ(0, sender.configure("reactor", "transport-test-reactor"));
        ASSERT_EQ(0, sender.configure("fec", "none"));
    }

    std::optional<bfc::shared_sized_buffer> next_wire()
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do
        {
            if (auto wire = tx->pop())
                return wire;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < until);
        return std::nullopt;
    }

    void post_custom_report(const stream_link_report &report,
                            bool legacy = false)
    {
        uint8_t wire[k_stream_header_len + k_stream_link_report_payload_len]{};
        stream_header hdr{};
        hdr.sequence_number = report.report_seq;
        stream_header_write(wire, hdr);
        stream_link_report_encode_payload(report, wire + k_stream_header_len,
                                          k_stream_link_report_payload_len);
        size_t len = sizeof(wire);
        if (legacy)
        {
            std::memmove(wire + k_stream_header_len,
                         wire + k_stream_header_len + 4,
                         k_stream_link_report_v1_payload_len);
            len = k_stream_header_len + k_stream_link_report_v1_payload_len;
        }
        (void)rx->push(bfc::shared_sized_buffer::copy_from(wire, len));
        reactor->wake_up();
    }

    void post_report(uint16_t seq)
    {
        uint8_t wire[k_stream_header_len + k_stream_link_report_payload_len]{};
        stream_header hdr{};
        hdr.sequence_number = seq;
        stream_header_write(wire, hdr);
        stream_link_report rep{};
        rep.session_id = 42;
        rep.timestamp_us = 1'000'000ULL + seq * 100'000ULL;
        rep.counters.udp_packet_received = seq;
        stream_link_report_encode_payload(rep, wire + k_stream_header_len,
                                          k_stream_link_report_payload_len);
        (void)rx->push(bfc::shared_sized_buffer::copy_from(wire, sizeof(wire)));
        reactor->wake_up();
    }

    bool wait_report(stream_sender &sender, uint16_t seq)
    {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do
        {
            const auto snap = vstreamer::test_query::peer(sender);
            if (snap.have && snap.report.counters.udp_packet_received == seq)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < until);
        return false;
    }
};

TEST_F(QueueTransportTest, RejectsMixedModesAndOldKeys)
{
    stream_sender sender;
    EXPECT_EQ(-ENOTSUP, sender.configure("stream", "127.0.0.1:9"));
    EXPECT_EQ(-ENOTSUP, sender.configure("reactor_name", "old"));
    EXPECT_EQ(-EINVAL, sender.open());
    ASSERT_EQ(0, sender.configure("udp_transport", "127.0.0.1:9"));
    EXPECT_EQ(-EINVAL,
              sender.configure("queue_transport_tx", "transport-test-tx"));
    EXPECT_EQ(-EINVAL,
              sender.configure("queue_transport_rx", "transport-test-rx"));
    ASSERT_EQ(0, sender.configure("udp_transport", ""));
    ASSERT_EQ(0, sender.configure("queue_transport_tx", "transport-test-tx"));
    EXPECT_EQ(-EINVAL, sender.open());
    EXPECT_EQ(-EINVAL, sender.configure("udp_transport", "127.0.0.1:9"));
    ASSERT_EQ(0, sender.configure("queue_transport_tx", ""));
    ASSERT_EQ(0, sender.configure("queue_transport_rx", "transport-test-rx"));
    EXPECT_EQ(-EINVAL, sender.open());
    EXPECT_EQ(-EINVAL, sender.configure("udp_transport", "127.0.0.1:9"));
}

TEST_F(QueueTransportTest, ValidatesRegistryEntriesAndCapacity)
{
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.configure("queue_transport_tx", "missing"));
    EXPECT_EQ(-ENOENT, sender.open());
    ASSERT_EQ(0, sender.configure("queue_transport_tx", "transport-test-tx"));
    ASSERT_EQ(0, sender.configure("queue_transport_rx", "missing"));
    EXPECT_EQ(-ENOENT, sender.open());
    ASSERT_EQ(0, sender.configure("queue_transport_rx", "transport-test-tx"));
    EXPECT_EQ(-EINVAL, sender.open());
    ASSERT_EQ(0, sender.configure("queue_transport_rx", "transport-test-rx"));
    ASSERT_EQ(0, sender.configure("reactor", ""));
    EXPECT_EQ(-EINVAL, sender.open());
    ASSERT_EQ(0, sender.configure("reactor", "missing"));
    EXPECT_EQ(-ENOENT, sender.open());
    ASSERT_EQ(0, sender.configure("reactor", "transport-test-reactor"));
    tx->set_capacity(0);
    EXPECT_EQ(-EINVAL, sender.open());
    tx->set_capacity(64);
    rx->set_capacity(0);
    EXPECT_EQ(-EINVAL, sender.open());
    rx->set_capacity(64);
    EXPECT_EQ(0, sender.open());
}

TEST_F(QueueTransportTest, RawPacketsPreservePayloadAndWireSequence)
{
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.open());
    uint16_t previous = 0;
    for (uint32_t tag = 1; tag <= 4; ++tag)
    {
        auto pdu = test_pdu::make_stream_dgram(tag, 64);
        const auto expected = pdu.sdu;
        ASSERT_EQ(0, sender.input(std::move(pdu)));
        auto wire = next_wire();
        ASSERT_TRUE(wire);
        uint16_t seq = 0;
        const uint8_t *payload = nullptr;
        size_t len = 0;
        ASSERT_TRUE(data_payload(*wire, &seq, &payload, &len));
        ASSERT_EQ(expected.size(), len);
        EXPECT_EQ(0, std::memcmp(expected.u8(), payload, len));
        if (tag > 1)
        {
            EXPECT_EQ(static_cast<uint16_t>(previous + 1), seq);
        }
        previous = seq;
    }
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (vstreamer::test_query::tuple(sender, "pkt_sent").first < 4 &&
           std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(4u, vstreamer::test_query::tuple(sender, "pkt_sent").first);
    EXPECT_EQ(4u * (64u + rs_block_erasure::k_data_header_len),
              vstreamer::test_query::tuple(sender, "byt_sent").first);
    for (auto key : {"udp_transport", "queue_transport_tx",
                     "queue_transport_rx", "reactor"})
        EXPECT_EQ(-EBUSY, sender.configure(key, ""));
}

TEST_F(QueueTransportTest, QueueOverflowRetainsNewestWirePackets)
{
    tx->set_capacity(2);
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.open());
    for (uint32_t tag = 1; tag <= 5; ++tag)
        ASSERT_EQ(0, sender.input(test_pdu::make_stream_dgram(tag, 16)));
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (vstreamer::test_query::tuple(sender, "pkt_sent").first < 5 &&
           std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(5u, vstreamer::test_query::tuple(sender, "pkt_sent").first);
    ASSERT_EQ(2u, tx->size());
    std::string dropped;
    ASSERT_EQ(0, sender.query("transport_dropped", &dropped));
    EXPECT_EQ("3", dropped);
    for (uint32_t tag : {4u, 5u})
    {
        auto wire = tx->pop();
        ASSERT_TRUE(wire);
        uint16_t seq = 0;
        const uint8_t *payload = nullptr;
        size_t len = 0;
        ASSERT_TRUE(data_payload(*wire, &seq, &payload, &len));
        EXPECT_EQ(tag, test_pdu::stream_dgram_counter_be32(payload, len));
    }
}

TEST_F(QueueTransportTest, ReportsDrainOnReactorAndCloseLeavesHostAlive)
{
    stream_sender sender;
    configure(sender);
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        ASSERT_EQ(0, sender.open());
        post_report(1);
        post_report(2);
        ASSERT_TRUE(wait_report(sender, 2));
        EXPECT_EQ(2u, vstreamer::test_query::peer(sender)
                          .report.counters.udp_packet_received);
        sender.close();
        auto alive = std::make_shared<std::promise<void>>();
        auto ready = alive->get_future();
        reactor->wake_up([alive] { alive->set_value(); });
        ASSERT_EQ(std::future_status::ready,
                  ready.wait_for(std::chrono::seconds(2)));
        post_report(3);
        EXPECT_FALSE(vstreamer::test_query::peer(sender).have);
        // Retained buffers belong to the host; remove the test report before
        // restart.
        ASSERT_TRUE(rx->pop());
    }
}

TEST_F(QueueTransportTest, TxPushWakesRegisteredConsumer)
{
    struct consumer_state
    {
        std::weak_ptr<stream_transport_queue> queue;
        std::promise<uint32_t> received;
        bool complete = false;
    };
    auto state = std::make_shared<consumer_state>();
    state->queue = tx;
    auto received = state->received.get_future();
    ASSERT_TRUE(reactor->add_read_rdy(
        *tx,
        [state]
        {
            auto queue = state->queue.lock();
            if (!queue)
                return;
            while (auto wire = queue->pop())
            {
                uint16_t seq = 0;
                const uint8_t *payload = nullptr;
                size_t len = 0;
                if (!state->complete && data_payload(*wire, &seq, &payload, &len))
                {
                    state->complete = true;
                    state->received.set_value(
                        test_pdu::stream_dgram_counter_be32(payload, len));
                }
            }
        }));
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.input(test_pdu::make_stream_dgram(123, 16)));
    ASSERT_EQ(std::future_status::ready,
              received.wait_for(std::chrono::seconds(2)));
    EXPECT_EQ(123u, received.get());
    // Host retains ownership of its TX consumer.
    sender.close();
    reactor->remove_read_rdy(*tx);
}

TEST_F(QueueTransportTest, QueriesAndTransportSwitchWhileClosed)
{
    vstreamer::default_reactor_host udp_reactor;
    stream_sender sender;
    configure(sender);
    std::string value;
    ASSERT_EQ(0, sender.query("queue_transport_tx", &value));
    EXPECT_EQ("transport-test-tx", value);
    ASSERT_EQ(0, sender.query("queue_transport_rx", &value));
    EXPECT_EQ("transport-test-rx", value);
    ASSERT_EQ(0, sender.query("udp_transport", &value));
    EXPECT_TRUE(value.empty());
    ASSERT_EQ(0, sender.query("reactor", &value));
    EXPECT_EQ("transport-test-reactor", value);
    ASSERT_EQ(0, sender.open());
    sender.close();
    ASSERT_EQ(0, sender.configure("queue_transport_tx", ""));
    EXPECT_EQ(-EINVAL, sender.configure("udp_transport", "127.0.0.1:9"));
    ASSERT_EQ(0, sender.configure("queue_transport_rx", ""));
    ASSERT_EQ(0, sender.configure("reactor", udp_reactor.name()));
    ASSERT_EQ(0, sender.configure("udp_transport", "127.0.0.1:9"));
    ASSERT_EQ(0, sender.open());
    sender.close();
}

TEST_F(QueueTransportTest, TelemetryOffLeavesRxQueueUntouched)
{
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.configure("telemetry", "off"));
    ASSERT_EQ(0, sender.open());
    post_report(1);
    auto idle = std::make_shared<std::promise<void>>();
    auto ready = idle->get_future();
    reactor->wake_up([idle] { idle->set_value(); });
    ASSERT_EQ(std::future_status::ready,
              ready.wait_for(std::chrono::seconds(2)));
    EXPECT_EQ(1u, rx->size());
    EXPECT_FALSE(vstreamer::test_query::peer(sender).have);
    sender.close();
}

TEST_F(QueueTransportTest, FecWirePacketsRecoverLostShard)
{
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.configure("fec", "block"));
    ASSERT_EQ(0, sender.configure("fec_k", "3"));
    ASSERT_EQ(0, sender.configure("fec_n", "4"));
    ASSERT_EQ(0, sender.open());
    for (uint32_t tag = 1; tag <= 3; ++tag)
        ASSERT_EQ(0, sender.input(test_pdu::make_stream_dgram(tag, 64)));
    rs_block_erasure decoder;
    fec_rx_payload_list decoded;
    for (int i = 0; i < 4; ++i)
    {
        auto wire = next_wire();
        ASSERT_TRUE(wire);
        rs_block_erasure::shard_header h;
        ASSERT_TRUE(rs_block_erasure::parse_header(wire->u8(), wire->size(), &h));
        EXPECT_EQ(i < 3, h.is_data);
        // Lose an actual on-wire data shard and reconstruct it using parity.
        if (i != 1)
        {
            fec_rx_payload_list step;
            decoder.push_air(wire->u8(), wire->size(), &step);
            decoded.insert(decoded.end(), std::make_move_iterator(step.begin()),
                           std::make_move_iterator(step.end()));
        }
    }
    ASSERT_EQ(3u, decoded.size());
    for (size_t i = 0; i < decoded.size(); ++i)
        EXPECT_EQ(i + 1, test_pdu::stream_dgram_counter_be32(
                             decoded[i].u8(), decoded[i].size()));
}

TEST_F(QueueTransportTest, UnifiedLocalMetricsCountPayloadAndParitySeparately)
{
    for (const char *mode : {"none", "block"})
    {
        stream_sender sender;
        configure(sender);
        ASSERT_EQ(0, sender.configure("fec", mode));
        if (std::string(mode) == "block")
        {
            ASSERT_EQ(0, sender.configure("fec_k", "3"));
            ASSERT_EQ(0, sender.configure("fec_n", "5"));
        }
        ASSERT_EQ(0, sender.open());
        const auto before_ms = stream_metric_timestamp_ms();
        size_t payload_bytes = 0;
        for (uint32_t tag = 1; tag <= 3; ++tag)
        {
            const size_t len = 13 + tag * 7;
            payload_bytes += len;
            ASSERT_EQ(0, sender.input(test_pdu::make_stream_dgram(tag, len)));
        }
        const size_t wire_packets = std::string(mode) == "block" ? 5 : 3;
        size_t wire_bytes = 0;
        for (size_t i = 0; i < wire_packets; ++i)
        {
            const auto wire = next_wire();
            ASSERT_TRUE(wire);
            wire_bytes += wire->size();
        }
        // next_wire can pop just before the sender commits its counters.
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (test_query::tuple(sender, "pkt_sent").first < wire_packets &&
               std::chrono::steady_clock::now() < until)
        {
            std::this_thread::yield();
        }
        for (int read = 0; read < 3; ++read)
        {
            const auto packets = test_query::tuple(sender, "pkt_sent");
            const auto bytes = test_query::tuple(sender, "byt_sent");
            EXPECT_GE(packets.timestamp_ms, before_ms);
            EXPECT_LE(bytes.timestamp_ms, stream_metric_timestamp_ms());
            EXPECT_EQ(wire_packets, packets.first);
            EXPECT_EQ(3u, packets.second);
            EXPECT_EQ(wire_bytes, bytes.first);
            EXPECT_EQ(payload_bytes, bytes.second);
        }
        std::string value;
        EXPECT_EQ(-EINVAL, sender.query("pkt_sent", nullptr));
        EXPECT_EQ(-ENOTSUP, sender.query("not_a_metric", &value));
    }
}

TEST_F(QueueTransportTest, PeerTuplesSupportLegacyAndVersionedReports)
{
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.open());
    std::string value = "unchanged";
    for (const char *key :
         {"peer_udp_pkt_recv", "peer_fec_pkt_recv", "peer_byt_recv"})
    {
        EXPECT_EQ(-ENODATA, sender.query(key, &value));
        EXPECT_EQ("unchanged", value);
        EXPECT_EQ(-EINVAL, sender.query(key, nullptr));
    }
    stream_link_report report;
    report.session_id = 12;
    report.report_seq = 1;
    report.timestamp_us = 9'876'543;
    report.counters = {11, 13, 17, 19, 10'001, 9'003};
    post_custom_report(report, true);
    ASSERT_TRUE(wait_report(sender, 11));
    ASSERT_EQ(0, sender.query("peer_udp_pkt_recv", &value));
    EXPECT_EQ("9876,11,17", value);
    ASSERT_EQ(0, sender.query("peer_fec_pkt_recv", &value));
    EXPECT_EQ("9876,13,19", value);
    EXPECT_EQ(-ENODATA, sender.query("peer_byt_recv", &value));

    report.report_seq = 2;
    report.counters.udp_packet_received = 22;
    post_custom_report(report);
    ASSERT_TRUE(wait_report(sender, 22));
    for (int read = 0; read < 3; ++read)
    {
        ASSERT_EQ(0, sender.query("peer_byt_recv", &value));
        EXPECT_EQ("9876,10001,9003", value);
    }
    // A later legacy report must not reuse byte totals from a preceding V2
    // report.
    report.report_seq = 3;
    report.counters.udp_packet_received = 33;
    post_custom_report(report, true);
    ASSERT_TRUE(wait_report(sender, 33));
    EXPECT_EQ(-ENODATA, sender.query("peer_byt_recv", &value));
}

TEST_F(QueueTransportTest, PeerTuplesStayConsistentDuringConcurrentReports)
{
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.open());
    std::atomic<bool> finished{false};
    std::thread producer(
        [&]
        {
            for (uint16_t seq = 1; seq <= 1000; ++seq)
            {
                stream_link_report report;
                report.session_id = 42;
                report.report_seq = seq;
                report.timestamp_us = static_cast<uint64_t>(seq) * 1000 + 999;
                report.counters = {seq * 11u, seq * 13u,  seq * 17u,
                                   seq * 19u, seq * 101u, seq * 103u};
                post_custom_report(report);
                std::this_thread::yield();
            }
            finished = true;
        });
    size_t observations = 0;
    do
    {
        for (const char *key :
             {"peer_udp_pkt_recv", "peer_fec_pkt_recv", "peer_byt_recv"})
        {
            stream_metric_tuple tuple;
            if (query_stream_metric(sender, key, &tuple))
            {
                const auto ts = tuple.timestamp_ms;
                const bool udp = std::string_view(key) == "peer_udp_pkt_recv";
                const bool fec = std::string_view(key) == "peer_fec_pkt_recv";
                EXPECT_EQ(ts * (udp ? 11 : fec ? 13 : 101), tuple.first);
                EXPECT_EQ(ts * (udp ? 17 : fec ? 19 : 103), tuple.second);
                ++observations;
            }
        }
    } while (!finished);
    producer.join();
    EXPECT_GT(observations, 0u);
}

#if defined(VSTREAMER_APP_TX_OK)
TEST_F(QueueTransportTest,
       PeerMetricsRetainValuesWithoutDataAndResetLossOnNewSession)
{
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.open());
    auto &metrics = apps::g_pipeline_metrics;
    metric_store(*metrics.get_metric("stream_sender.peer_udp_packet_received"),
                 uint64_t{123});
    apps::tx::sync_sender_peer_link_metrics_live(sender);
    std::string value;
    ASSERT_TRUE(metrics.format_metric("stream_sender.peer_udp_packet_received",
                                      &value));
    EXPECT_EQ("123", value);
    stream_link_report report;
    report.session_id = 567;
    report.report_seq = 1;
    report.timestamp_us = 1000;
    report.counters = {1, 1, 0, 0, 10, 10};
    post_custom_report(report);
    ASSERT_TRUE(wait_report(sender, 1));
    apps::tx::sync_sender_peer_link_metrics_live(sender);
    report.report_seq = 2;
    report.timestamp_us = 2000;
    report.counters = {11, 11, 10, 10, 110, 110};
    post_custom_report(report);
    ASSERT_TRUE(wait_report(sender, 11));
    EXPECT_DOUBLE_EQ(50., apps::tx::sender_peer_fec_loss_pct(sender));
    // Counts increase and timestamps collide, so only the session identifies
    // the reset.
    report.session_id = 568;
    report.report_seq = 0;
    report.counters = {101, 101, 100, 100, 1010, 1010};
    post_custom_report(report);
    ASSERT_TRUE(wait_report(sender, 101));
    EXPECT_DOUBLE_EQ(0., apps::tx::sender_peer_fec_loss_pct(sender));
}
#endif
}  // namespace
