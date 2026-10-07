#ifndef VSTREAMER_APPS_STAGE_LATENCY_HPP
#define VSTREAMER_APPS_STAGE_LATENCY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/component_pdu.hpp"

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

void stage_latency_set_diag_enabled(bool enabled);

[[nodiscard]] bool stage_latency_stderr_enabled();

[[nodiscard]] int stage_latency_stderr_stride();

void note_source_pdu(const vstreamer::component_pdu &pdu);
void record_stage_latency_ms(const char *stage, double ms);

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_STAGE_LATENCY_HPP
