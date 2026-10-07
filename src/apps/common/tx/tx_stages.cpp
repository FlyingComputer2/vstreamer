/* tx_stages.cpp — PDU-native stage threads (legacy packet API at encoder/pay bridge). */

#include "apps/common/tx/tx_stages.hpp"

#include "apps/common/legacy_pdu.hpp"
#include "apps/common/pdu_stage.hpp"
#include "apps/common/pipeline_state.hpp"
#include "apps/common/stage_latency.hpp"
#include "apps/common/tx/source_selector.hpp"
#include "apps/common/tx/tx_state.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "components/components.hpp"
#include "core/data_packet.hpp"
#include "core/pdu_input.hpp"
#include "core/pdu_output.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/thread_affinity.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;
using apps::g_cpu_map;
using apps::g_run;
using apps::log_pdu_stage_latency;
using apps::log_stage_latency;
using apps::note_pdu_sequence_gap;
using apps::note_source_pdu;
using apps::pipeline_pdu_queue;
using apps::tx::g_tx;
using apps::wait_for_pdu;
using apps::record_pdu_edge_latency_ms;

#if !defined(VSTREAMER_BENCH_RX_ONLY)

namespace
{

[[nodiscard]] size_t pdu_bytes(const component_pdu &pdu)
{
    return is_caps(pdu.sdu_type) ? 0U : pdu.sdu.size();
}

[[nodiscard]] pipeline_pdu_queue *route_q(const component_pdu &pdu, pipeline_pdu_queue *mjpeg_pipe,
                                          pipeline_pdu_queue *nv12_pipe)
{
    if (pdu.sdu_type == sdu_type_e::NV12 || pdu.sdu_type == sdu_type_e::CAPS_VIDEO_RAW)
    {
        return nv12_pipe;
    }
    if (pdu.sdu_type == sdu_type_e::MJPEG || pdu.sdu_type == sdu_type_e::CAPS_VIDEO_CODED)
    {
        return mjpeg_pipe;
    }
    return nullptr;
}

void apply_pending_console_encoder_cfg(apps::tx::encoder_t &enc)
{
    const int kbps = g_tx.pending_console_cbr_kbps.exchange(-1, std::memory_order_acq_rel);
    if (kbps >= 100)
    {
        char bps_buf[32];
        std::snprintf(bps_buf, sizeof(bps_buf), "%d", kbps * 1000);
        (void)enc.configure("cbr", std::string_view(bps_buf));
    }
    const int qp = g_tx.pending_console_qp.exchange(-1, std::memory_order_acq_rel);
    if (qp >= 0 && qp <= 51)
    {
        char qp_buf[16];
        std::snprintf(qp_buf, sizeof(qp_buf), "%d", qp);
        (void)enc.configure("qp", std::string_view(qp_buf));
    }
    const int gop = g_tx.pending_console_gop.exchange(-1, std::memory_order_acq_rel);
    if (gop >= 1 && gop <= 255)
    {
        char gop_buf[16];
        std::snprintf(gop_buf, sizeof(gop_buf), "%d", gop);
        (void)enc.configure("gop", std::string_view(gop_buf));
    }
    if (g_tx.pending_console_idr.exchange(false, std::memory_order_acq_rel))
    {
        (void)enc.configure("idr", "");
    }
}

bool handle_source_poll_error(int got)
{
    if (-EBADF == got || -ECANCELED == got)
    {
        return false;
    }
    if (-EAGAIN == got)
    {
        return true;
    }
    std::fprintf(stderr, "stream_sdl: source output failed (%d)\n", got);
    return false;
}

bool forward_encoded_au(rtp_h264_pay &pay, stream_sender &sender, bench_diag &diag, data_packet &pkt)
{
    apps::log_stage_latency("enc_out", pkt);
    if (pay.input(0, pkt) < 0)
    {
        return false;
    }
    bool sent = false;
    data_packet sock_pkt;
    while (g_run.load(std::memory_order_relaxed) && pay.output(0, sock_pkt, 0) == 0)
    {
        if (sender.input(0, sock_pkt) == 0)
        {
            diag.tx_rtp_sock++;
            sent = true;
        }
    }
    return sent;
}

void forward_aus_and_note_emitted(rtp_h264_pay &pay, stream_sender &sender, bench_diag &diag,
                                  std::vector<data_packet> &aus)
{
    for (data_packet &pkt : aus)
    {
        const frame_data &f = data_packet::cast<frame_data>(pkt);
        diag.tx_enc_out_bytes.fetch_add(f.buf.size(), std::memory_order_relaxed);
        (void)forward_encoded_au(pay, sender, diag, pkt);
    }
}

void pull_encoded_aus(apps::tx::encoder_t &enc, std::vector<data_packet> &out)
{
    data_packet pkt;
    while (g_run.load(std::memory_order_relaxed) && enc.output(0, pkt, 0) == 0)
    {
        out.push_back(std::move(pkt));
    }
}

void drain_encoder(apps::tx::encoder_t &enc, rtp_h264_pay &pay, stream_sender &sender,
                   bench_diag &diag)
{
    std::vector<data_packet> aus;
    pull_encoded_aus(enc, aus);
    forward_aus_and_note_emitted(pay, sender, diag, aus);
}

bool submit_nv12_to_encoder(apps::tx::encoder_t *enc, rtp_h264_pay *pay, stream_sender *sender,
                            bench_diag *diag, data_packet &nv12, bool *accepted)
{
    if (nullptr != accepted)
    {
        *accepted = false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    bool       got_enc_in = false;
    while (g_run.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline)
    {
        const int enc_in = enc->input(0, nv12);
        if (0 == enc_in)
        {
            diag->tx_nv12++;
            got_enc_in = true;
            apps::log_stage_latency("enc_in", nv12);
            break;
        }
        if (-ECANCELED == enc_in)
        {
            return false;
        }
        if (-EAGAIN == enc_in)
        {
            drain_encoder(*enc, *pay, *sender, *diag);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        diag->tx_enc_in_err++;
        break;
    }
    if (!got_enc_in)
    {
        diag->tx_enc_input_miss++;
    }
    else if (nullptr != accepted)
    {
        *accepted = true;
    }
    drain_encoder(*enc, *pay, *sender, *diag);
    return true;
}

}  // namespace

bool enqueue_source_pdu(component_pdu &&raw, pipeline_pdu_queue *mjpeg_pipe, pipeline_pdu_queue *nv12_pipe,
                        bench_diag *diag)
{
    diag->tx_noise++;
    diag->tx_source_bytes += pdu_bytes(raw);
    note_source_pdu(raw);
    log_pdu_stage_latency("source", raw);
    pipeline_pdu_queue *q = route_q(raw, mjpeg_pipe, nv12_pipe);
    if (nullptr == q)
    {
        return true;
    }
    if (q == nv12_pipe)
    {
        diag->tx_jpeg_nv12++;
        diag->tx_jpeg_nv12_bytes += pdu_bytes(raw);
        return q->push(std::move(raw), &diag->tx_nv12_q_drop);
    }
    return q->push(std::move(raw), &diag->tx_mjpeg_q_drop);
}

void source_stage_main(component_source *source, pipeline_pdu_queue *mjpeg_pipe, pipeline_pdu_queue *nv12_pipe,
                       bench_diag *diag)
{
    pin_current_thread_to_cpu(g_cpu_map.source);
    auto *po = dynamic_cast<pdu_output *>(source);
    auto *owner = dynamic_cast<component *>(source);
    if (nullptr == po || nullptr == owner)
    {
        apps::legacy_to_pdu to_pdu;
        while (g_run.load(std::memory_order_relaxed))
        {
            data_packet raw;
            const int got = source->output(0, raw, g_run.load() ? -1 : 0);
            if (got < 0)
            {
                if (!handle_source_poll_error(got))
                {
                    break;
                }
                continue;
            }
            std::vector<component_pdu> pdus;
            to_pdu.convert(raw, &pdus);
            for (component_pdu &pdu : pdus)
            {
                (void)enqueue_source_pdu(std::move(pdu), mjpeg_pipe, nv12_pipe, diag);
            }
        }
        return;
    }
    std::shared_ptr<pdu_wakeup> wake = std::make_shared<pdu_wakeup>();
    owner->set_wakeup(wake);
    while (g_run.load(std::memory_order_relaxed))
    {
        component_pdu pdu;
        const int     got = po->output(pdu);
        if (0 == got)
        {
            (void)enqueue_source_pdu(std::move(pdu), mjpeg_pipe, nv12_pipe, diag);
            continue;
        }
        if (-EAGAIN == got)
        {
            wait_for_pdu(*wake, *owner, g_run);
            continue;
        }
        if (!handle_source_poll_error(got))
        {
            break;
        }
    }
}

void source_stage_selector_main(apps::tx::source_selector *selector, pipeline_pdu_queue * /*mjpeg_pipe*/,
                                pipeline_pdu_queue * /*nv12_pipe*/, bench_diag * /*diag*/)
{
    pin_current_thread_to_cpu(g_cpu_map.source);
    while (g_run.load(std::memory_order_relaxed) && nullptr != selector)
    {
        const int got = selector->poll_once(g_run.load(std::memory_order_relaxed) ? -1 : 0);
        if (got < 0 && !handle_source_poll_error(got))
        {
            break;
        }
    }
}

void jpeg_stage_main(jpeg_decoder_multicore *jdec, pipeline_pdu_queue *mjpeg_pipe, pipeline_pdu_queue *nv12_pipe,
                     bench_diag *diag, int max_inflight)
{
    pin_current_thread_to_cpu(g_cpu_map.jpeg);
    if (max_inflight < 1)
    {
        max_inflight = 1;
    }
    auto *jdec_in = dynamic_cast<pdu_input *>(jdec);
    auto *jdec_out = dynamic_cast<pdu_output *>(jdec);
    auto *owner = dynamic_cast<component *>(jdec);
    if (nullptr == jdec_in || nullptr == jdec_out || nullptr == owner)
    {
        return;
    }
    std::shared_ptr<pdu_wakeup> wake = mjpeg_pipe->shared_wakeup();
    if (!wake)
    {
        wake = std::make_shared<pdu_wakeup>();
        mjpeg_pipe->bind_wakeup(wake);
    }
    owner->set_wakeup(wake);
    uint64_t last_seq = 0;
    bool     have_seq = false;
    int      inflight = 0;
    component_pdu raw;
    bool          holding = false;
    while (g_run.load(std::memory_order_relaxed))
    {
        component_pdu out;
        while (jdec_out->output(out) == 0)
        {
            inflight--;
            note_pdu_sequence_gap(out.seq, last_seq, have_seq, nullptr);
            log_pdu_stage_latency("jpeg_nv12", out);
            diag->tx_jpeg_nv12++;
            diag->tx_jpeg_nv12_bytes += pdu_bytes(out);
            (void)nv12_pipe->push(std::move(out), &diag->tx_nv12_q_drop);
        }
        while (inflight < max_inflight)
        {
            if (!holding)
            {
                if (!mjpeg_pipe->pop(raw, *wake, *owner))
                {
                    break;
                }
                holding = true;
            }
            if (is_caps(raw.sdu_type))
            {
                (void)jdec_in->input(std::move(raw));
                holding = false;
                continue;
            }
            const int ir = jdec_in->input(std::move(raw));
            if (0 == ir)
            {
                holding = false;
                inflight++;
                continue;
            }
            if (-EAGAIN == ir)
            {
                break;
            }
            holding = false;
            break;
        }
        if (!holding && inflight < max_inflight)
        {
            wait_for_pdu(*wake, *owner, g_run);
        }
    }
}

void encode_stage_main(apps::tx::encoder_t *enc, rtp_h264_pay *pay, stream_sender *sender,
                       pipeline_pdu_queue *nv12_pipe, bench_diag *diag)
{
    pin_current_thread_to_cpu(g_cpu_map.encode);
    auto *enc_pdu_in = dynamic_cast<pdu_input *>(enc);
    auto *owner = dynamic_cast<component *>(enc);
    if (nullptr == owner)
    {
        return;
    }
    std::shared_ptr<pdu_wakeup> pipe_wake = nv12_pipe->shared_wakeup();
    if (!pipe_wake)
    {
        pipe_wake = std::make_shared<pdu_wakeup>();
        nv12_pipe->bind_wakeup(pipe_wake);
    }
    std::shared_ptr<pdu_wakeup> wake = pipe_wake;
    owner->set_wakeup(wake);
    pay->set_wakeup(wake);
    sender->set_wakeup(wake);
    std::unordered_map<uint64_t, int64_t> queue_in_mono_ns;
    nv12_pipe->bind_enqueue_mono_tracker(&queue_in_mono_ns);
    apps::pdu_to_legacy                   to_legacy;
    component_pdu                         nv12_pdu;
    data_packet                           nv12;
    bool                                  holding = false;
    uint64_t                              last_seq = 0;
    bool                                  have_seq = false;
    while (g_run.load(std::memory_order_relaxed))
    {
        apply_pending_console_encoder_cfg(*enc);
        if (!holding)
        {
            if (!nv12_pipe->pop(nv12_pdu, *wake, *owner))
            {
                drain_encoder(*enc, *pay, *sender, *diag);
                continue;
            }
            diag->tx_enc_nv12_popped++;
            note_pdu_sequence_gap(nv12_pdu.seq, last_seq, have_seq, nullptr);
            if (is_caps(nv12_pdu.sdu_type))
            {
                /* Keep pdu_to_legacy caps state in sync with enc PDU caps. */
                (void)to_legacy.convert(nv12_pdu, &nv12);
                if (nullptr != enc_pdu_in)
                {
                    (void)enc_pdu_in->input(std::move(nv12_pdu));
                }
                drain_encoder(*enc, *pay, *sender, *diag);
                continue;
            }
            record_pdu_edge_latency_ms("enc_in", nv12_pdu, &queue_in_mono_ns);
            log_pdu_stage_latency("enc_in", nv12_pdu);
            const int cr = to_legacy.convert(nv12_pdu, &nv12);
            if (0 != cr)
            {
                continue;
            }
            holding = true;
        }
        bool accepted = false;
        if (!submit_nv12_to_encoder(enc, pay, sender, diag, nv12, &accepted))
        {
            break;
        }
        if (accepted)
        {
            holding = false;
            nv12.release();
            continue;
        }
        drain_encoder(*enc, *pay, *sender, *diag);
        wait_for_pdu(*wake, *owner, g_run);
    }
}

#endif

}  // namespace vstreamer::test_app
