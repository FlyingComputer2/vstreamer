#ifndef VSTREAMER_APPS_PDU_STAGE_HPP
#define VSTREAMER_APPS_PDU_STAGE_HPP

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "apps/common/stage_latency.hpp"
#include "core/component.hpp"
#include "core/component_pdu.hpp"
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

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_PDU_STAGE_HPP
