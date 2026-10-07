#include "apps/common/pdu_stage.hpp"

#include <atomic>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/sequence_gap.hpp"
#include "core/sdu_caps.hpp"

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
        record_stage_latency_ms(stage, ms);
    }
    if (!stage_latency_stderr_enabled())
    {
        return;
    }
    const int stride = stage_latency_stderr_stride();
    if (stride > 1 && (pdu.ts_us % static_cast<uint64_t>(stride)) != 0)
    {
        return;
    }
    std::fprintf(stderr, "stage_latency: %-10s %7.2f ms ts_us=%" PRIu64 "\n", stage, ms,
                 pdu.ts_us);
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

void record_pdu_edge_latency_ms(const char *stage, const component_pdu &pdu,
                                std::unordered_map<uint64_t, int64_t> *edge_mono_by_ts_us)
{
    if (nullptr == stage || nullptr == edge_mono_by_ts_us || is_caps(pdu.sdu_type) || pdu.ts_us == 0)
    {
        return;
    }
    const auto it = edge_mono_by_ts_us->find(pdu.ts_us);
    if (it == edge_mono_by_ts_us->end())
    {
        return;
    }
    const int64_t now_ns = steady_mono_ns();
    const double  ms = static_cast<double>(now_ns - it->second) / 1e6;
    if (ms >= 0.0)
    {
        record_stage_latency_ms(stage, ms);
    }
    edge_mono_by_ts_us->erase(it);
}

void note_pdu_sequence_gap(uint64_t seq, uint64_t &last_seq, bool &have_last,
                           std::atomic<uint64_t> *gap_counter)
{
    const uint64_t gap = note_forward_gap(seq, last_seq, have_last);
    if (gap > 0 && nullptr != gap_counter)
    {
        gap_counter->fetch_add(gap, std::memory_order_relaxed);
    }
}

void wait_for_pdu(pdu_wakeup &w, component &deadline_owner, const std::atomic<bool> &stop_flag)
{
    if (stop_flag.load(std::memory_order_relaxed))
    {
        return;
    }
    const int64_t now_ns = steady_mono_ns();
    int64_t       deadline = deadline_owner.next_deadline_ns();
    const int64_t cap = now_ns + k_pdu_stage_wait_cap_ns;
    if (deadline > cap)
    {
        deadline = cap;
    }
    if (deadline <= now_ns)
    {
        deadline = cap;
    }
    w.wait_until(deadline);
}

}  // namespace vstreamer::apps
