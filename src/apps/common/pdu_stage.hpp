#ifndef VSTREAMER_APPS_PDU_STAGE_HPP
#define VSTREAMER_APPS_PDU_STAGE_HPP

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "apps/common/stage_latency.hpp"
#include "core/component.hpp"
#include "core/component_pdu.hpp"
#include "core/component_output.hpp"
#include "core/pdu_wakeup.hpp"
#include "core/time_util.hpp"

namespace vstreamer::apps
{

constexpr int64_t k_pdu_stage_wait_cap_ns = 50'000'000LL;

void log_pdu_stage_latency(const char *stage, const component_pdu &pdu);

void note_pdu_input_ts(uint64_t ts_us, int64_t mono_ns,
                       std::unordered_map<uint64_t, int64_t> *input_mono_by_ts_us);

void record_pdu_edge_latency_ms(const char *stage, const component_pdu &pdu,
                                std::unordered_map<uint64_t, int64_t> *edge_mono_by_ts_us);

void note_pdu_sequence_gap(uint64_t seq, uint64_t &last_seq, bool &have_last,
                           std::atomic<uint64_t> *gap_counter);

void wait_for_pdu(pdu_wakeup &w, component &deadline_owner, const std::atomic<bool> &stop_flag);

template <typename SinkFn>
void pump_pdu_output(component_output &src, component &owner, pdu_wakeup &w, std::atomic<bool> &stop_flag,
                     const char *latency_stage, SinkFn &&sink,
                     std::unordered_map<uint64_t, int64_t> *input_mono_by_ts_us)
{
    component_pdu held;
    bool            holding = false;

    while (!stop_flag.load(std::memory_order_relaxed))
    {
        if (!holding)
        {
            component_pdu out_pdu;
            const int oret = src.output(out_pdu);
            if (0 == oret)
            {
                if (nullptr != latency_stage)
                {
                    log_pdu_stage_latency(latency_stage, out_pdu);
                }
                if (nullptr != input_mono_by_ts_us && !is_caps(out_pdu.sdu_type) &&
                    out_pdu.ts_us > 0)
                {
                    const auto it = input_mono_by_ts_us->find(out_pdu.ts_us);
                    if (it != input_mono_by_ts_us->end())
                    {
                        const int64_t now_ns = steady_mono_ns();
                        const double  node_ms =
                            static_cast<double>(now_ns - it->second) / 1e6;
                        record_stage_latency_ms(latency_stage, node_ms);
                        input_mono_by_ts_us->erase(it);
                    }
                }
                const int sret = sink(out_pdu);
                if (-EAGAIN == sret)
                {
                    held = std::move(out_pdu);
                    holding = true;
                }
            }
            else if (-EAGAIN != oret)
            {
                break;
            }
        }
        else
        {
            const int sret = sink(held);
            if (0 == sret)
            {
                holding = false;
            }
            else if (-EAGAIN != sret)
            {
                break;
            }
        }

        const int64_t now_ns = steady_mono_ns();
        int64_t       deadline = owner.next_deadline_ns();
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
}

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_PDU_STAGE_HPP
