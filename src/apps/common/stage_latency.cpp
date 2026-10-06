#include "apps/common/stage_latency.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/data_packet.hpp"
#include "core/time_util.hpp"

namespace vstreamer::apps
{

std::atomic<int64_t> g_latest_source_pts {0};
std::atomic<double>  g_glass_latency_ms {0.0};
std::atomic<double>  g_latency_source_ms {0.0};
std::atomic<double>  g_latency_jpeg_ms {0.0};
std::atomic<double>  g_latency_enc_in_ms {0.0};
std::atomic<double>  g_latency_enc_out_ms {0.0};
std::atomic<double>  g_latency_depay_ms {0.0};
std::atomic<double>  g_latency_dec_in_ms {0.0};
std::atomic<double>  g_latency_dec_out_ms {0.0};
std::atomic<double>  g_latency_present_ms {0.0};

std::atomic<double> g_node_latency_source_ms {0.0};
std::atomic<double> g_node_latency_jpeg_ms {0.0};
std::atomic<double> g_node_latency_encoder_queue_ms {0.0};
std::atomic<double> g_node_latency_enc_in_ms {0.0};
std::atomic<double> g_node_latency_enc_out_ms {0.0};
std::atomic<double> g_node_latency_depay_ms {0.0};
std::atomic<double> g_node_latency_dec_in_ms {0.0};
std::atomic<double> g_node_latency_dec_out_ms {0.0};
std::atomic<double> g_node_latency_present_ms {0.0};

namespace
{

std::atomic<bool> g_stage_latency_diag {false};

[[nodiscard]] bool stage_latency_log_enabled()
{
    static const bool env_on = [] {
        const char *v = std::getenv("VSTREAMER_LOG_STAGE_LATENCY");
        return nullptr != v && v[0] != '\0' && 0 != std::strcmp(v, "0");
    }();
    return g_stage_latency_diag.load(std::memory_order_relaxed) || env_on;
}

[[nodiscard]] int stage_latency_log_stride()
{
    static const int every = [] {
        const char *v = std::getenv("VSTREAMER_STAGE_LATENCY_EVERY");
        if (nullptr == v || v[0] == '\0')
        {
            return 1;
        }
        char       *end = nullptr;
        const long n = std::strtol(v, &end, 10);
        return (end != v && n > 0) ? static_cast<int>(n) : 1;
    }();
    return every;
}

}  // namespace

void stage_latency_set_diag_enabled(bool enabled)
{
    g_stage_latency_diag.store(enabled, std::memory_order_relaxed);
}

void note_source_pts(const data_packet &pkt)
{
    if (pkt.get_type() != packet_kind_e::FRAME)
    {
        return;
    }
    const frame_data &f = data_packet::cast<frame_data>(pkt);
    g_latest_source_pts.store(f.pts, std::memory_order_relaxed);
}

[[nodiscard]] size_t packet_frame_bytes(const data_packet &pkt)
{
    if (pkt.get_type() != packet_kind_e::FRAME)
    {
        return 0;
    }
    return data_packet::cast<frame_data>(pkt).buf.size();
}

[[nodiscard]] media_kind_e packet_media_kind(const data_packet &pkt)
{
    if (pkt.get_type() != packet_kind_e::FRAME)
    {
        return media_kind_e::UNKNOWN;
    }
    return data_packet::cast<frame_data>(pkt).kind;
}

void record_stage_latency_ms(const char *stage, const data_packet &, double ms)
{
    if (nullptr == stage)
    {
        return;
    }
    if (0 == std::strcmp(stage, "source"))
    {
        g_latency_source_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "jpeg_nv12"))
    {
        g_latency_jpeg_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "enc_in"))
    {
        g_latency_enc_in_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "enc_out"))
    {
        g_latency_enc_out_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "depay"))
    {
        g_latency_depay_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "dec_in"))
    {
        g_latency_dec_in_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "dec_out"))
    {
        g_latency_dec_out_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "present"))
    {
        g_latency_present_ms.store(ms, std::memory_order_relaxed);
        g_glass_latency_ms.store(ms, std::memory_order_relaxed);
    }
}

double mono_interval_ms(int64_t input_mono_ns, int64_t output_mono_ns)
{
    if (input_mono_ns <= 0 || output_mono_ns <= input_mono_ns)
    {
        return 0.0;
    }
    return static_cast<double>(output_mono_ns - input_mono_ns) / 1e6;
}

void record_stage_node_latency_ms(const char *stage, int64_t input_mono_ns, int64_t output_mono_ns)
{
    if (nullptr == stage)
    {
        return;
    }
    const double ms = mono_interval_ms(input_mono_ns, output_mono_ns);
    if (ms <= 0.0)
    {
        return;
    }
    if (0 == std::strcmp(stage, "source"))
    {
        g_node_latency_source_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "jpeg_nv12"))
    {
        g_node_latency_jpeg_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "encoder_queue"))
    {
        g_node_latency_encoder_queue_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "enc_in"))
    {
        g_node_latency_enc_in_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "enc_out"))
    {
        g_node_latency_enc_out_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "depay"))
    {
        g_node_latency_depay_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "dec_in"))
    {
        g_node_latency_dec_in_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "dec_out"))
    {
        g_node_latency_dec_out_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "present"))
    {
        g_node_latency_present_ms.store(ms, std::memory_order_relaxed);
    }
}

void log_stage_latency(const char *stage, const data_packet &pkt)
{
    if (nullptr == stage || pkt.get_type() != packet_kind_e::FRAME)
    {
        return;
    }
    const frame_data &f = data_packet::cast<frame_data>(pkt);
    if (f.capture_mono_ns <= 0)
    {
        return;
    }
    const int64_t now_ns = steady_mono_ns();
    const double  ms = static_cast<double>(now_ns - f.capture_mono_ns) / 1e6;
    if (ms >= 0.0)
    {
        record_stage_latency_ms(stage, pkt, ms);
    }

    if (!stage_latency_log_enabled())
    {
        return;
    }
    const int stride = stage_latency_log_stride();
    if (stride > 1 && (f.pts % stride) != 0)
    {
        return;
    }
    std::fprintf(stderr, "stage_latency: %-10s %7.2f ms pts=%" PRId64 "\n", stage, ms, f.pts);
}

}  // namespace vstreamer::apps
