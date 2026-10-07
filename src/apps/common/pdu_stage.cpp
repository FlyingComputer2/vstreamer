#include "apps/common/pdu_stage.hpp"

#include <cmath>

namespace vstreamer::apps
{

void log_pdu_stage_latency(const char *stage, const component_pdu &pdu)
{
    if (nullptr == stage || is_caps(pdu.sdu_type) || pdu.ts_us <= 0)
    {
        return;
    }
    const int64_t now_ns = steady_mono_ns();
    const int64_t cap_ns = static_cast<int64_t>(pdu.ts_us) * 1000LL;
    const double  ms = static_cast<double>(now_ns - cap_ns) / 1e6;
    if (ms >= 0.0)
    {
        record_stage_latency_ms(stage, data_packet(), ms);
    }
}

void note_pdu_input_ts(uint64_t ts_us, int64_t mono_ns,
                       std::unordered_map<uint64_t, int64_t> *input_mono_by_ts_us)
{
    if (nullptr == input_mono_by_ts_us || 0 == ts_us)
    {
        return;
    }
    (*input_mono_by_ts_us)[ts_us] = mono_ns;
}

}  // namespace vstreamer::apps
