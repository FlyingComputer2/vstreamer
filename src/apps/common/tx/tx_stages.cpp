/* tx_stages.cpp — transmit-half pipeline stages. */

#include "apps/common/tx/tx_stages.hpp"

#include "apps/common/stage_latency.hpp"
#include "apps/common/tx/source_selector.hpp"
#include "apps/stream_sdl_test/encoder_types.hpp"
#include "apps/stream_sdl_test/pipeline_state.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "components/components.hpp"
#include "core/data_packet.hpp"
#include "core/thread_affinity.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;
using apps::log_stage_latency;
using apps::note_source_pts;
using apps::packet_frame_bytes;
using apps::packet_media_kind;

#if !defined(VSTREAMER_BENCH_RX_ONLY)

bool forward_encoded_au(rtp_h264_pay &pay, stream_sender &sender, bench_diag &diag,
                        data_packet &pkt)
{
    log_stage_latency("enc_out", pkt);
    if (pay.input(0, pkt) < 0)
    {
        return false;
    }
    bool sent = false;
    data_packet sock_pkt;
    while (g_run.load() && pay.output(0, sock_pkt, 0) == 0)
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

void pull_encoded_aus(h264_encoder_t &enc, std::vector<data_packet> &out)
{
    data_packet pkt;
    while (g_run.load() && enc.output(0, pkt, 0) == 0)
    {
        out.push_back(std::move(pkt));
    }
}

void drain_encoder(h264_encoder_t &enc, rtp_h264_pay &pay, stream_sender &sender, bench_diag &diag)
{
    std::vector<data_packet> aus;
    pull_encoded_aus(enc, aus);
    forward_aus_and_note_emitted(pay, sender, diag, aus);
}

bool submit_nv12_to_encoder(h264_encoder_t *enc, rtp_h264_pay *pay, stream_sender *sender,
                            bench_diag *diag, data_packet &nv12, bool *accepted)
{
    if (nullptr != accepted)
    {
        *accepted = false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    bool       got_enc_in = false;
    while (g_run.load() && std::chrono::steady_clock::now() < deadline)
    {
        const int enc_in = enc->input(0, nv12);
        if (0 == enc_in)
        {
            diag->tx_nv12++;
            got_enc_in = true;
            log_stage_latency("enc_in", nv12);
            break;
        }
        if (-ECANCELED == enc_in)
        {
            return false;
        }
        if (-EAGAIN == enc_in)
        {
            std::vector<data_packet> aus;
            pull_encoded_aus(*enc, aus);
            forward_aus_and_note_emitted(*pay, *sender, *diag, aus);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        diag->tx_enc_in_err++;
        const uint64_t n = diag->tx_enc_in_err.load();
        if (n <= 8)
        {
            std::fprintf(stderr, "stream_sdl: h264_encoder input failed (%d", enc_in);
            if (-enc_in > 0 && -enc_in < 4096)
            {
                std::fprintf(stderr, "; %s", std::strerror(-enc_in));
            }
            std::fprintf(stderr, ")\n");
        }
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
    std::vector<data_packet> tail;
    pull_encoded_aus(*enc, tail);
    forward_aus_and_note_emitted(*pay, *sender, *diag, tail);
    return true;
}

namespace
{

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
    std::fprintf(stderr, "stream_sdl: source output failed (%d", got);
    if (-got > 0 && -got < 4096)
    {
        std::fprintf(stderr, "; %s", std::strerror(-got));
    }
    std::fprintf(stderr, ")\n");
    return false;
}

}  // namespace

bool enqueue_source_frame(data_packet &&raw, apps::pipeline_queue *mjpeg_q, apps::pipeline_queue *nv12_q,
                          bench_diag *diag)
{
    diag->tx_noise++;
    diag->tx_source_bytes += packet_frame_bytes(raw);
    note_source_pts(raw);
    log_stage_latency("source", raw);
    if (packet_media_kind(raw) == media_kind_e::NV12)
    {
        diag->tx_jpeg_nv12++;
        diag->tx_jpeg_nv12_bytes += packet_frame_bytes(raw);
        return nv12_q->push(std::move(raw), &diag->tx_nv12_q_drop);
    }
    return mjpeg_q->push(std::move(raw), &diag->tx_mjpeg_q_drop);
}

void source_stage_main(component_source *source, apps::pipeline_queue *mjpeg_q, apps::pipeline_queue *nv12_q,
                       bench_diag *diag)
{
    pin_current_thread_to_cpu(g_cpu_map.source);
    while (g_run.load())
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
        data_packet moved = std::move(raw);
        if (!enqueue_source_frame(std::move(moved), mjpeg_q, nv12_q, diag))
        {
            break;
        }
    }
}

void source_stage_selector_main(apps::tx::source_selector *selector, apps::pipeline_queue * /*mjpeg_q*/,
                                apps::pipeline_queue * /*nv12_q*/, bench_diag * /*diag*/)
{
    pin_current_thread_to_cpu(g_cpu_map.source);
    while (g_run.load() && nullptr != selector)
    {
        const int got = selector->poll_once(g_run.load() ? -1 : 0);
        if (got < 0 && !handle_source_poll_error(got))
        {
            break;
        }
    }
}

void emit_jpeg_nv12(apps::pipeline_queue *nv12_q, bench_diag *diag, data_packet &nv12)
{
    diag->tx_jpeg_nv12++;
    diag->tx_jpeg_nv12_bytes += packet_frame_bytes(nv12);
    log_stage_latency("jpeg_nv12", nv12);
    (void)nv12_q->push(std::move(nv12), &diag->tx_nv12_q_drop);
}

void jpeg_stage_main(jpeg_decoder_multicore *jdec, apps::pipeline_queue *mjpeg_q, apps::pipeline_queue *nv12_q,
                     bench_diag *diag, int max_inflight)
{
    pin_current_thread_to_cpu(g_cpu_map.jpeg);
    if (max_inflight < 1)
    {
        max_inflight = 1;
    }
    int         inflight = 0;
    data_packet raw;
    bool        holding_raw = false;
    while (g_run.load())
    {
        data_packet nv12;
        while (g_run.load())
        {
            const int or_out = jdec->output(0, nv12, 0);
            if (0 == or_out)
            {
                inflight--;
                emit_jpeg_nv12(nv12_q, diag, nv12);
                continue;
            }
            if (-EBADF == or_out || -ECANCELED == or_out)
            {
                return;
            }
            if (-EAGAIN == or_out)
            {
                break;
            }
            std::fprintf(stderr, "stream_sdl: jpeg_decoder output failed (%d)\n", or_out);
            break;
        }

        while (g_run.load() && inflight < max_inflight)
        {
            if (!holding_raw)
            {
                const int pop_ms = (inflight == 0 && !holding_raw) ? 50 : 0;
                if (!mjpeg_q->pop(raw, pop_ms))
                {
                    break;
                }
                holding_raw = true;
            }
            const int ir = jdec->input(0, raw);
            if (0 == ir)
            {
                holding_raw = false;
                inflight++;
                continue;
            }
            if (-EBADF == ir || -ECANCELED == ir)
            {
                return;
            }
            if (-EAGAIN == ir)
            {
                break;
            }
            std::fprintf(stderr, "stream_sdl: jpeg_decoder input failed (%d)\n", ir);
            holding_raw = false;
            break;
        }

        if (!holding_raw && inflight < max_inflight)
        {
            continue;
        }

        const int or_out = jdec->output(0, nv12, 50);
        if (0 == or_out)
        {
            inflight--;
            emit_jpeg_nv12(nv12_q, diag, nv12);
            continue;
        }
        if (-EBADF == or_out || -ECANCELED == or_out)
        {
            return;
        }
    }
}

void apply_pending_console_encoder_cfg(h264_encoder_t &enc)
{
    const int kbps = g_tx.pending_console_cbr_kbps.exchange(-1, std::memory_order_acq_rel);
    if (kbps >= 100)
    {
        char bps_buf[32];
        std::snprintf(bps_buf, sizeof(bps_buf), "%d", kbps * 1000);
        std::string_view val = bps_buf;
        (void)enc.configure("cbr", val);
    }
    const int qp = g_tx.pending_console_qp.exchange(-1, std::memory_order_acq_rel);
    if (qp >= 0 && qp <= 51)
    {
        char qp_buf[16];
        std::snprintf(qp_buf, sizeof(qp_buf), "%d", qp);
        std::string_view val = qp_buf;
        (void)enc.configure("qp", val);
    }
    const int gop = g_tx.pending_console_gop.exchange(-1, std::memory_order_acq_rel);
    if (gop >= 1 && gop <= 255)
    {
        char gop_buf[16];
        std::snprintf(gop_buf, sizeof(gop_buf), "%d", gop);
        std::string_view val = gop_buf;
        (void)enc.configure("gop", val);
    }
    if (g_tx.pending_console_idr.exchange(false, std::memory_order_acq_rel))
    {
        (void)enc.configure("idr", "");
    }
}

void encode_stage_main(h264_encoder_t *enc, rtp_h264_pay *pay, stream_sender *sender,
                       apps::pipeline_queue *nv12_q, bench_diag *diag)
{
    pin_current_thread_to_cpu(g_cpu_map.encode);
    data_packet nv12;
    bool        holding = false;
    while (g_run.load())
    {
        apply_pending_console_encoder_cfg(*enc);
        if (!holding)
        {
            if (!nv12_q->pop(nv12, 50))
            {
                drain_encoder(*enc, *pay, *sender, *diag);
                continue;
            }
            diag->tx_enc_nv12_popped++;
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
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}


#endif

}  // namespace vstreamer::test_app
