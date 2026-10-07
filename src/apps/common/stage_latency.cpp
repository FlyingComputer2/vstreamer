#include "apps/common/stage_latency.hpp"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"

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

namespace
{

std::atomic<bool> g_stage_latency_diag {false};

[[nodiscard]] bool stage_latency_log_env_enabled()
{
    static const bool env_on = [] {
        const char *v = std::getenv("VSTREAMER_LOG_STAGE_LATENCY");
        return nullptr != v && v[0] != '\0' && 0 != std::strcmp(v, "0");
    }();
    return env_on;
}

}  // namespace

void stage_latency_set_diag_enabled(bool enabled)
{
    g_stage_latency_diag.store(enabled, std::memory_order_relaxed);
}

bool stage_latency_stderr_enabled()
{
    return g_stage_latency_diag.load(std::memory_order_relaxed) || stage_latency_log_env_enabled();
}

int stage_latency_stderr_stride()
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

void note_source_pdu(const component_pdu &pdu)
{
    if (is_caps(pdu.sdu_type) || pdu.ts_us == 0)
    {
        return;
    }
    g_latest_source_pts.store(static_cast<int64_t>(pdu.ts_us), std::memory_order_relaxed);
}

void record_stage_latency_ms(const char *stage, double ms)
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

}  // namespace vstreamer::apps
