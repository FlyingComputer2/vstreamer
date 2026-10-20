#include "core/stream_queue_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <future>
#include <iterator>
#include <optional>
#include <thread>

#include "components/stream_sender.hpp"
#include "core/stream_header.hpp"
#include "core/stream_telemetry.hpp"
#include "test_pdu_helpers.hpp"

namespace
{
using namespace vstreamer;

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
            const auto snap = sender.peer_link_snapshot();
            if (snap.have && snap.report.report_seq == seq)
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
    ASSERT_EQ(0, sender.set_enabled(true, 0));
    uint16_t previous = 0;
    for (uint32_t tag = 1; tag <= 4; ++tag)
    {
        auto pdu = test_pdu::make_stream_dgram(tag, 64);
        const auto expected = pdu.sdu;
        ASSERT_EQ(0, sender.input(std::move(pdu)));
        auto wire = next_wire();
        ASSERT_TRUE(wire);
        stream_header hdr{};
        const uint8_t *payload = nullptr;
        size_t len = 0;
        ASSERT_EQ(0, stream_header_parse(wire->u8(), wire->size(), &hdr,
                                         &payload, &len));
        EXPECT_TRUE(hdr.is_stream_data);
        EXPECT_FALSE(hdr.is_fec);
        ASSERT_EQ(expected.size(), len);
        EXPECT_EQ(0, std::memcmp(expected.u8(), payload, len));
        if (tag > 1)
        {
            EXPECT_EQ(static_cast<uint16_t>(previous + 1), hdr.sequence_number);
        }
        previous = hdr.sequence_number;
    }
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sender.wire_pkts_sent_counter().load() < 4 &&
           std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(4u, sender.wire_pkts_sent_counter().load());
    EXPECT_EQ(4u * (64u + k_stream_header_len),
              sender.wire_bytes_sent_counter().load());
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
    ASSERT_EQ(0, sender.set_enabled(true, 0));
    for (uint32_t tag = 1; tag <= 5; ++tag)
        ASSERT_EQ(0, sender.input(test_pdu::make_stream_dgram(tag, 16)));
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sender.wire_pkts_sent_counter().load() < 5 &&
           std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(5u, sender.wire_pkts_sent_counter().load());
    ASSERT_EQ(2u, tx->size());
    for (uint32_t tag : {4u, 5u})
    {
        auto wire = tx->pop();
        ASSERT_TRUE(wire);
        stream_header hdr{};
        const uint8_t *payload = nullptr;
        size_t len = 0;
        ASSERT_EQ(0, stream_header_parse(wire->u8(), wire->size(), &hdr,
                                         &payload, &len));
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
        EXPECT_EQ(
            2u,
            sender.peer_link_snapshot().report.counters.udp_packet_received);
        sender.close();
        auto alive = std::make_shared<std::promise<void>>();
        auto ready = alive->get_future();
        reactor->wake_up([alive] { alive->set_value(); });
        ASSERT_EQ(std::future_status::ready,
                  ready.wait_for(std::chrono::seconds(2)));
        post_report(3);
        EXPECT_FALSE(sender.peer_link_snapshot().have);
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
                stream_header hdr{};
                const uint8_t *payload = nullptr;
                size_t len = 0;
                if (!state->complete &&
                    stream_header_parse(wire->u8(), wire->size(), &hdr,
                                        &payload, &len) == 0)
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
    ASSERT_EQ(0, sender.set_enabled(true, 0));
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
    ASSERT_EQ(0, sender.configure("reactor", ""));
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
    EXPECT_FALSE(sender.peer_link_snapshot().have);
    sender.close();
}

TEST_F(QueueTransportTest, FecWirePacketsRecoverLostShard)
{
    stream_sender sender;
    configure(sender);
    ASSERT_EQ(0, sender.configure("fec", "block"));
    ASSERT_EQ(0, sender.configure("fec_k", "3"));
    ASSERT_EQ(0, sender.configure("fec_n", "4"));
    ASSERT_EQ(0, sender.configure("fec_timeout_ms", "1000"));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));
    for (uint32_t tag = 1; tag <= 3; ++tag)
        ASSERT_EQ(0, sender.input(test_pdu::make_stream_dgram(tag, 64)));
    rs_block_erasure decoder;
    fec_rx_payload_list decoded;
    for (int i = 0; i < 4; ++i)
    {
        auto wire = next_wire();
        ASSERT_TRUE(wire);
        stream_header hdr{};
        const uint8_t *payload = nullptr;
        size_t len = 0;
        ASSERT_EQ(0, stream_header_parse(wire->u8(), wire->size(), &hdr,
                                         &payload, &len));
        ASSERT_TRUE(hdr.is_fec);
        // Lose an actual on-wire data shard and reconstruct it using parity.
        if (i != 1)
        {
            fec_rx_payload_list step;
            decoder.push_air(payload, len, &step);
            decoded.insert(decoded.end(), std::make_move_iterator(step.begin()),
                           std::make_move_iterator(step.end()));
        }
    }
    ASSERT_EQ(3u, decoded.size());
    for (size_t i = 0; i < decoded.size(); ++i)
        EXPECT_EQ(i + 1, test_pdu::stream_dgram_counter_be32(
                             decoded[i].u8(), decoded[i].size()));
}
}  // namespace
