#ifndef VSTREAMER_APPS_STAGE_LATENCY_HPP
#define VSTREAMER_APPS_STAGE_LATENCY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/data_packet.hpp"

namespace vstreamer::apps
{

extern std::atomic<int64_t> g_latest_source_pts;
extern std::atomic<double>  g_glass_latency_ms;
extern std::atomic<double>  g_latency_source_ms;
extern std::atomic<double>  g_latency_jpeg_ms;
extern std::atomic<double>  g_latency_enc_in_ms;
extern std::atomic<double>  g_latency_enc_out_ms;
extern std::atomic<double>  g_latency_depay_ms;
extern std::atomic<double>  g_latency_dec_in_ms;
extern std::atomic<double>  g_latency_dec_out_ms;
extern std::atomic<double>  g_latency_present_ms;

extern std::atomic<double> g_node_latency_source_ms;
extern std::atomic<double> g_node_latency_jpeg_ms;
extern std::atomic<double> g_node_latency_encoder_queue_ms;
extern std::atomic<double> g_node_latency_enc_in_ms;
extern std::atomic<double> g_node_latency_enc_out_ms;
extern std::atomic<double> g_node_latency_depay_ms;
extern std::atomic<double> g_node_latency_dec_in_ms;
extern std::atomic<double> g_node_latency_dec_out_ms;
extern std::atomic<double> g_node_latency_present_ms;

void stage_latency_set_diag_enabled(bool enabled);

void note_source_pts(const vstreamer::data_packet &pkt);
[[nodiscard]] size_t packet_frame_bytes(const vstreamer::data_packet &pkt);
[[nodiscard]] vstreamer::media_kind_e packet_media_kind(const vstreamer::data_packet &pkt);
void record_stage_latency_ms(const char *stage, const vstreamer::data_packet &, double ms);
void log_stage_latency(const char *stage, const vstreamer::data_packet &pkt);

/* Per-node delay from monotonic input/output stamps on the same frame (ms). */
[[nodiscard]] double mono_interval_ms(int64_t input_mono_ns, int64_t output_mono_ns);

void record_stage_node_latency_ms(const char *stage, int64_t input_mono_ns, int64_t output_mono_ns);

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_STAGE_LATENCY_HPP
