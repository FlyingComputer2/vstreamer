/* stage_latency_metrics_test.cpp — each app half publishes only the latency keys it measures. */

#include "apps/common/pipeline_state.hpp"
#include "apps/common/rx/rx_metrics.hpp"
#include "apps/common/stage_latency.hpp"
#include "apps/common/tx/tx_metrics.hpp"

#include <gtest/gtest.h>

#include <string>

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

}  // namespace
