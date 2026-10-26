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

/* Node entry stamps (steady mono ns) keyed by ts_us, for nodes that hold several frames at once.
 * Owned by one stage thread. */
using pdu_stamp_map = std::unordered_map<uint64_t, int64_t>;

/* Stamps ts_us entering a node; a frame already stamped keeps its first stamp. Stamps whose
 * output never appears (dropped frames) expire after 1 s; at the size cap the oldest goes. */
void note_pdu_input_ts(uint64_t ts_us, int64_t mono_ns, pdu_stamp_map *input_mono_by_ts_us);

/* Stores exit − entry for pdu.ts_us into *node and drops the stamp. */
void record_pdu_edge_latency_ms(std::atomic<double> *node, const component_pdu &pdu,
                                pdu_stamp_map *edge_mono_by_ts_us);

void note_pdu_sequence_gap(uint64_t seq, uint64_t &last_seq, bool &have_last,
                           std::atomic<uint64_t> *gap_counter);

void wait_for_pdu(pdu_wakeup &w, component &deadline_owner, const std::atomic<bool> &run_flag);

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_PDU_STAGE_HPP
