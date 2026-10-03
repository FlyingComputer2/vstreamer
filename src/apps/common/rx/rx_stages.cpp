/* rx_stages.cpp — SA-T4 split from stream_sdl stages. */

#include "apps/common/rx/rx_stages.hpp"

#include "apps/common/stage_latency.hpp"
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
using apps::packet_frame_bytes;

#if !defined(VSTREAMER_BENCH_TX_ONLY)

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
                    const int   fps = g_tx.stream_fps.load(std::memory_order_relaxed);
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
            if (g_rx.skip_decode.load())
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
        if (g_rx.skip_decode.load())
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


#endif

}  // namespace vstreamer::test_app
