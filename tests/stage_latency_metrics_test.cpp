/* stage_latency_metrics_test.cpp — each app half publishes only the latency keys it measures. */

#include "apps/common/pdu_stage.hpp"
#include "apps/common/pipeline_state.hpp"
#include "apps/common/rx/rx_metrics.hpp"
#include "apps/common/stage_latency.hpp"
#include "apps/common/tx/tx_metrics.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace
{

using vstreamer::apps::g_pipeline_metrics;

/* Runs before the TX test in this file: the metrics registry is process-wide and keys cannot be
 * removed, so the TX publish must not have created them yet. ctest runs each test on its own. */
TEST(StageLatencyMetricsTest, RxPublishOmitsTxStageKeys)
{
    vstreamer::apps::rx::publish_latency_metrics(12.0);

    std::string v;
    EXPECT_TRUE(g_pipeline_metrics.format_metric("latency.glass_ms", &v));
    EXPECT_TRUE(g_pipeline_metrics.format_metric("latency.depay_ms", &v));
    EXPECT_FALSE(g_pipeline_metrics.format_metric("latency.source_ms", &v));
    EXPECT_FALSE(g_pipeline_metrics.format_metric("latency.jpeg_ms", &v));
    EXPECT_FALSE(g_pipeline_metrics.format_metric("latency.enc_in_ms", &v));
    EXPECT_FALSE(g_pipeline_metrics.format_metric("latency.enc_out_ms", &v));
    EXPECT_TRUE(g_pipeline_metrics.format_metric("latency.dec_node_ms", &v));
    EXPECT_FALSE(g_pipeline_metrics.format_metric("latency.enc_node_ms", &v));
}

TEST(StageLatencyMetricsTest, TxPublishCarriesTxStageValues)
{
    vstreamer::apps::g_latency_source_ms.store(3.5);
    vstreamer::apps::g_latency_jpeg_ms.store(41.5);
    vstreamer::apps::g_latency_enc_in_ms.store(44.5);
    vstreamer::apps::g_latency_enc_out_ms.store(77.5);
    vstreamer::apps::tx::publish_tx_latency_metrics(nullptr);

    std::string v;
    ASSERT_TRUE(g_pipeline_metrics.format_metric("latency.source_ms", &v));
    EXPECT_NE(std::string::npos, v.find("3.5")) << v;
    ASSERT_TRUE(g_pipeline_metrics.format_metric("latency.jpeg_ms", &v));
    EXPECT_NE(std::string::npos, v.find("41.5")) << v;
    ASSERT_TRUE(g_pipeline_metrics.format_metric("latency.enc_in_ms", &v));
    EXPECT_NE(std::string::npos, v.find("44.5")) << v;
    ASSERT_TRUE(g_pipeline_metrics.format_metric("latency.enc_out_ms", &v));
    EXPECT_NE(std::string::npos, v.find("77.5")) << v;
}

TEST(StageLatencyMetricsTest, TxPublishCarriesNodeValues)
{
    vstreamer::apps::g_node_latency_jpeg_ms.store(6.5);
    vstreamer::apps::g_node_latency_encoder_queue_ms.store(2.5);
    vstreamer::apps::g_node_latency_enc_ms.store(9.5);
    vstreamer::apps::tx::publish_tx_latency_metrics(nullptr);

    std::string v;
    ASSERT_TRUE(g_pipeline_metrics.format_metric("latency.jpeg_node_ms", &v));
    EXPECT_NE(std::string::npos, v.find("6.5")) << v;
    ASSERT_TRUE(g_pipeline_metrics.format_metric("encoder_queue.node_latency_ms", &v));
    EXPECT_NE(std::string::npos, v.find("2.5")) << v;
    ASSERT_TRUE(g_pipeline_metrics.format_metric("h264_encoder.node_latency_ms", &v));
    EXPECT_NE(std::string::npos, v.find("9.5")) << v;
}

TEST(StageLatencyMetricsTest, EdgeLatencyIsExitMinusEntryForSameTs)
{
    std::unordered_map<uint64_t, int64_t> stamps;
    const int64_t                         now_ns = vstreamer::steady_mono_ns();
    vstreamer::apps::note_pdu_input_ts(1000, now_ns - 5'000'000, &stamps);
    vstreamer::apps::note_pdu_input_ts(2000, now_ns - 50'000'000, &stamps);

    vstreamer::component_pdu out;
    out.ts_us = 1000;
    out.sdu_type = vstreamer::sdu_type_e::H264_AU;
    vstreamer::apps::g_node_latency_enc_ms.store(0.0);
    vstreamer::apps::record_pdu_edge_latency_ms("enc", out, &stamps);

    const double ms = vstreamer::apps::g_node_latency_enc_ms.load();
    EXPECT_GE(ms, 5.0);
    EXPECT_LT(ms, 50.0);
    EXPECT_EQ(1U, stamps.size());
    EXPECT_EQ(0U, stamps.count(1000));
}

TEST(StageLatencyMetricsTest, EdgeStampsForDroppedFramesArePruned)
{
    std::unordered_map<uint64_t, int64_t> stamps;
    const int64_t                         now_ns = vstreamer::steady_mono_ns();
    for (uint64_t ts = 1; ts <= 64; ts++)
    {
        vstreamer::apps::note_pdu_input_ts(ts, now_ns - 2'000'000'000LL, &stamps);
    }
    vstreamer::apps::note_pdu_input_ts(100, now_ns, &stamps);
    EXPECT_EQ(1U, stamps.size());
    EXPECT_EQ(1U, stamps.count(100));
}

}  // namespace
