/* stages.cpp — split from stream_sdl (P11-T4). */

#include "test_app/stream_sdl/stages.hpp"

#include "apps/common/stage_latency.hpp"
#include "test_app/stream_sdl/pipeline_state.hpp"

#include <cerrno>

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

int cfg_str(component &c, const char *key, const char *val)
{
    std::string_view k(key);
    std::string_view v(val);
    return c.configure(k, v);
}

int open_stage(const char *name, int rc)
{
    if (rc < 0)
    {
        std::fprintf(stderr, "open failed: %s (%d", name, rc);
        if (-rc > 0 && -rc < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-rc));
        }
        std::fprintf(stderr, ")\n");
    }
    return rc;
}

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
            if (-EBADF == got || -ECANCELED == got)
            {
                break;
            }
            if (-EAGAIN == got)
            {
                continue;
            }
            std::fprintf(stderr, "stream_sdl: source output failed (%d", got);
            if (-got > 0 && -got < 4096)
            {
                std::fprintf(stderr, "; %s", std::strerror(-got));
            }
            std::fprintf(stderr, ")\n");
            break;
        }
        diag->tx_noise++;
        diag->tx_source_bytes += packet_frame_bytes(raw);
        note_source_pts(raw);
        log_stage_latency("source", raw);
        if (packet_media_kind(raw) == media_kind_e::NV12)
        {
            diag->tx_jpeg_nv12++;
            diag->tx_jpeg_nv12_bytes += packet_frame_bytes(raw);
            if (!nv12_q->push(std::move(raw), &diag->tx_nv12_q_drop))
            {
                break;
            }
        }
        else if (!mjpeg_q->push(std::move(raw), &diag->tx_mjpeg_q_drop))
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
    const int kbps = g_pending_console_cbr_kbps.exchange(-1, std::memory_order_acq_rel);
    if (kbps >= 100)
    {
        char bps_buf[32];
        std::snprintf(bps_buf, sizeof(bps_buf), "%d", kbps * 1000);
        std::string_view val = bps_buf;
        (void)enc.configure("cbr", val);
    }
    const int qp = g_pending_console_qp.exchange(-1, std::memory_order_acq_rel);
    if (qp >= 0 && qp <= 51)
    {
        char qp_buf[16];
        std::snprintf(qp_buf, sizeof(qp_buf), "%d", qp);
        std::string_view val = qp_buf;
        (void)enc.configure("qp", val);
    }
    const int gop = g_pending_console_gop.exchange(-1, std::memory_order_acq_rel);
    if (gop >= 1 && gop <= 255)
    {
        char gop_buf[16];
        std::snprintf(gop_buf, sizeof(gop_buf), "%d", gop);
        std::string_view val = gop_buf;
        (void)enc.configure("gop", val);
    }
    if (g_pending_console_idr.exchange(false, std::memory_order_acq_rel))
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

int drain_decoder_one_frame(h264_decoder_mpp *dec, data_packet &frame_pkt, bench_diag &diag,
                            int timeout_ms)
{
    const int r = dec->output(0, frame_pkt, timeout_ms);
    if (0 == r)
    {
        diag.rx_nv12_out++;
        diag.rx_nv12_out_bytes += packet_frame_bytes(frame_pkt);
        return 0;
    }
    if (-EAGAIN == r)
    {
        diag.rx_dec_out_eagain++;
        return r;
    }
    diag.rx_dec_out_err++;
    if (diag.rx_dec_out_err <= 10)
    {
        std::fprintf(stderr, "stream_sdl: h264_decoder output failed (%d", r);
        if (-r > 0 && -r < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-r));
        }
        std::fprintf(stderr, ")\n");
    }
    return r;
}

void pull_decoder_frames_locked(h264_decoder_mpp *dec, bench_diag &diag,
                                std::vector<data_packet> &out)
{
    for (int pass = 0; pass < 64; pass++)
    {
        data_packet frame_pkt;
        const int   r = drain_decoder_one_frame(dec, frame_pkt, diag, 0);
        if (0 != r)
        {
            break;
        }
        out.push_back(std::move(frame_pkt));
    }
}

void enqueue_decoded_frames(apps::present_frame_queue *present_q, bench_diag &diag,
                            std::vector<data_packet> &frames)
{
    for (data_packet &frame_pkt : frames)
    {
        if (nullptr == present_q)
        {
            continue;
        }
        log_stage_latency("dec_out", frame_pkt);
        (void)present_q->push(std::move(frame_pkt), &diag.rx_present_q_drop);
    }
}

void drain_decoder_to_present(h264_decoder_mpp *dec, apps::present_frame_queue *present_q,
                              bench_diag &diag)
{
    if (ensure_decoder_open(dec) < 0)
    {
        return;
    }
    std::vector<data_packet> frames;
    pull_decoder_frames_locked(dec, diag, frames);
    enqueue_decoded_frames(present_q, diag, frames);
}

void present_thread_main(component_sink *display, apps::present_frame_queue *present_q, int width,
                         int height, bool kmsdrm, bool sdl_open_on_thread, bench_diag *diag)
{
    if (nullptr == display || nullptr == present_q || nullptr == diag)
    {
        return;
    }
    if (sdl_open_on_thread)
    {
        if (display->open() < 0)
        {
            std::fprintf(stderr, "stream_sdl: SDL display open on present thread failed\n");
            return;
        }
        if (prepare_preview_sink(display, kmsdrm, width, height) < 0)
        {
            std::fprintf(stderr,
                         "stream_sdl: SDL display prepare on present thread failed "
                         "(continuing)\n");
        }
    }
    else if (kmsdrm && prepare_preview_sink(display, kmsdrm, width, height) < 0)
    {
        std::fprintf(stderr,
                     "stream_sdl: kmsdrm prepare on present thread failed (continuing)\n");
    }

    while (g_run.load())
    {
        data_packet frame_pkt;
        if (!present_q->pop(frame_pkt, 50))
        {
            continue;
        }
        const int pr = display->input(0, frame_pkt);
        if (0 == pr)
        {
            diag->rx_present_ok++;
            log_stage_latency("present", frame_pkt);
            if (frame_pkt.get_type() == packet_kind_e::FRAME)
            {
                const frame_data &f = data_packet::cast<frame_data>(frame_pkt);
                if (f.capture_mono_ns <= 0)
                {
                    const int64_t latest =
                        apps::g_latest_source_pts.load(std::memory_order_relaxed);
                    const int   fps = g_stream_fps.load(std::memory_order_relaxed);
                    if (fps > 0 && latest >= f.pts)
                    {
                        const double lag_ms = static_cast<double>(latest - f.pts) * 1000.0 /
                                                static_cast<double>(fps);
                        apps::g_glass_latency_ms.store(lag_ms, std::memory_order_relaxed);
                        apps::g_latency_present_ms.store(lag_ms, std::memory_order_relaxed);
                    }
                }
            }
        }
        else
        {
            diag->rx_present_err++;
            const uint64_t n = diag->rx_present_err.load();
            if (n <= 12)
            {
                std::fprintf(stderr, "stream_sdl: display present failed (%d", pr);
                if (-pr > 0 && -pr < 4096)
                {
                    std::fprintf(stderr, "; %s", std::strerror(-pr));
                }
                std::string sink_stats;
                if (display->query("stats", &sink_stats) == 0)
                {
                    std::fprintf(stderr, "; sink{%.*s}", static_cast<int>(sink_stats.size()),
                                 sink_stats.data());
                }
                std::fprintf(stderr, ")\n");
            }
        }
    }

    data_packet tail;
    while (present_q->pop(tail, 0))
    {
        if (display->input(0, tail) == 0)
        {
            diag->rx_present_ok++;
        }
    }
}

int feed_decoder_au(h264_decoder_mpp *dec, data_packet &au, apps::present_frame_queue *present_q,
                    bench_diag &diag)
{
    if (!g_run.load())
    {
        return -ECANCELED;
    }
    if (ensure_decoder_open(dec) < 0)
    {
        return -EINVAL;
    }
    for (int attempt = 0; g_run.load() && attempt < 48; attempt++)
    {
        const int r = dec->input(0, au);
        if (0 == r)
        {
            diag.rx_dec_in_ok++;
            diag.rx_dec_in_bytes += packet_frame_bytes(au);
            return 0;
        }
        if (-ECANCELED == r)
        {
            return -ECANCELED;
        }
        if (-EAGAIN == r)
        {
            diag.rx_dec_in_eagain++;
            if (!g_run.load())
            {
                return -ECANCELED;
            }
            drain_decoder_to_present(dec, present_q, diag);
            continue;
        }
        diag.rx_dec_in_err++;
        if (diag.rx_dec_in_err <= 5)
        {
            const frame_data &f = data_packet::cast<frame_data>(au);
            std::fprintf(stderr,
                         "stream_sdl: h264_decoder input failed (%d) au_bytes=%zu\n", r,
                         f.buf.size());
        }
        return r;
    }
    return -EAGAIN;
}
int prepare_preview_sink(component_sink *preview, bool /*kmsdrm*/, int w, int h)
{
    return static_cast<sdl_sink *>(preview)->prepare(w, h);
}
void rx_net_thread_main(stream_receiver *rcv, rtp_h264_depay *depay, apps::rx_au_queue *au_q,
                        bench_diag *diag)
{
    auto depay_sock = [&](data_packet &sock_pkt) {
        diag->rx_udp++;
        const int dep = depay->input(0, sock_pkt);
        if (dep < 0)
        {
            diag->rx_depay_err++;
            if (diag->rx_depay_err <= 5)
            {
                std::fprintf(stderr, "stream_sdl: rtp depay input failed (%d)\n", dep);
            }
            return;
        }
        data_packet au;
        while (g_run.load() && depay->output(0, au, 0) == 0)
        {
            diag->rx_depay_au++;
            if (g_skip_decode.load())
            {
                continue;
            }
            log_stage_latency("depay", au);
            au_q->push(std::move(au), &diag->rx_au_q_drop);
        }
    };

    while (g_run.load())
    {
        data_packet sock_pkt;
        const int   got = rcv->output(0, sock_pkt, 50);
        if (0 == got)
        {
            depay_sock(sock_pkt);
            continue;
        }
        if (got < 0 && -EAGAIN != got)
        {
            break;
        }
    }
}

void decode_thread_main(h264_decoder_mpp *dec, apps::present_frame_queue *present_q, apps::rx_au_queue *au_q,
                        bench_diag *diag)
{
    pin_current_thread_to_cpu(g_cpu_map.rx);

    data_packet holding;
    bool        have_holding = false;
    while (g_run.load())
    {
        if (g_skip_decode.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (!have_holding)
        {
            if (!au_q->pop(holding, 50))
            {
                drain_decoder_to_present(dec, present_q, *diag);
                continue;
            }
            have_holding = true;
        }

        log_stage_latency("dec_in", holding);
        const int r = feed_decoder_au(dec, holding, present_q, *diag);
        if (0 == r)
        {
            have_holding = false;
            holding.release();
            drain_decoder_to_present(dec, present_q, *diag);
            continue;
        }
        if (-EAGAIN == r)
        {
            drain_decoder_to_present(dec, present_q, *diag);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        if (-ECANCELED == r)
        {
            break;
        }
        have_holding = false;
        holding.release();
    }

    drain_decoder_to_present(dec, present_q, *diag);
}

}  // namespace vstreamer::test_app
