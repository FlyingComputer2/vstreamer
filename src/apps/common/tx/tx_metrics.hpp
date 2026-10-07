#ifndef VSTREAMER_APPS_TX_TX_METRICS_HPP
#define VSTREAMER_APPS_TX_TX_METRICS_HPP

#include <string>

#include "apps/stream_sdl_test/diag.hpp"

#include "core/component_coder.hpp"
#include "core/component_source.hpp"
#include "core/stream_telemetry.hpp"

namespace vstreamer
{
class component;
class stream_sender;
}

namespace vstreamer::apps::tx
{

bool query_source_metric_string(vstreamer::component_source *src, const char *key,
                                std::string &out);
void store_source_pipeline_metrics(double source_out_fps, double source_out_kbps, const char *ts,
                                   vstreamer::component *jdec);

void store_sender_peer_link_metrics(uint64_t udp_recv, uint64_t fec_recv, uint64_t udp_gap,
                                    uint64_t fec_gap, double loss_udp_pct, double loss_fec_pct);
void sync_sender_peer_link_metrics_live(vstreamer::stream_sender &sender);
[[nodiscard]] double sender_peer_fec_loss_pct(vstreamer::stream_sender &sender);
/* Polls the sender's received link reports into stream_sender.peer_* every 100 ms. */
void telemetry_thread_main(vstreamer::stream_sender *sender);

int query_encoder_qp(vstreamer::component_coder &enc);
int query_encoder_cbr_bps(vstreamer::component_coder &enc);

void sync_tx_cumulative_counters(const vstreamer::test_app::bench_diag &d,
                                vstreamer::stream_sender *sender);

}  // namespace vstreamer::apps::tx

#endif
