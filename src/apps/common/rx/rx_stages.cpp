/* rx_stages.cpp — PDU-native RX stages (SDL sink uses PDU input from step 7). */

#include "apps/common/rx/rx_stages.hpp"

#include "apps/common/pdu_stage.hpp"
#include "apps/common/pipeline_state.hpp"
#include "apps/common/rx/rx_state.hpp"
#include "apps/common/stage_latency.hpp"

#include <cerrno>
#include <cstdio>
#include <chrono>
#include <memory>
#include <thread>

#include "components/components.hpp"
#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/thread_affinity.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;
using apps::g_cpu_map;
using apps::g_run;
using apps::log_pdu_stage_latency;
using apps::note_pdu_sequence_gap;
using apps::pdu_rx_au_queue;
using apps::present_pdu_queue;
using apps::rx::ensure_decoder_open;
using apps::rx::g_rx;
using apps::wait_for_pdu;

#if !defined(VSTREAMER_BENCH_TX_ONLY)

namespace
{

[[nodiscard]] size_t pdu_bytes(const component_pdu &pdu)
{
    return is_caps(pdu.sdu_type) ? 0U : pdu.sdu.size();
}

void drain_dec_pdus(h264_decoder_mpp *dec, present_pdu_queue *present_queue, bench_diag &diag)
{
    if (ensure_decoder_open(dec) < 0)
    {
        return;
    }
    auto *dec_out = dynamic_cast<component_output *>(dec);
    if (nullptr == dec_out)
    {
        return;
    }
    uint64_t last_seq = 0;
    bool     have_seq = false;
    component_pdu frame;
    while (g_run.load(std::memory_order_relaxed) && dec_out->output(frame) == 0)
    {
        note_pdu_sequence_gap(frame.seq, last_seq, have_seq, nullptr);
        diag.rx_nv12_out++;
        diag.rx_nv12_out_bytes += pdu_bytes(frame);
        log_pdu_stage_latency("dec_out", frame);
        (void)present_queue->push(std::move(frame), &diag.rx_present_q_drop);
    }
}

int feed_decoder_pdu(h264_decoder_mpp *dec, component_pdu &pdu, present_pdu_queue *present_queue,
                     bench_diag &diag)
{
    if (ensure_decoder_open(dec) < 0)
    {
        return -EINVAL;
    }
    auto *dec_in = dynamic_cast<component_input *>(dec);
    if (nullptr == dec_in)
    {
        return -ENOTSUP;
    }
    const size_t bytes = pdu.sdu.size();
    for (int attempt = 0; g_run.load(std::memory_order_relaxed) && attempt < 48; attempt++)
    {
        component_pdu attempt_pdu = pdu;
        const int     r = dec_in->input(std::move(attempt_pdu));
        if (0 == r)
        {
            diag.rx_dec_in_ok++;
            diag.rx_dec_in_bytes += bytes;
            return 0;
        }
        if (-ECANCELED == r)
        {
            return -ECANCELED;
        }
        if (-EAGAIN == r)
        {
            diag.rx_dec_in_eagain++;
            drain_dec_pdus(dec, present_queue, diag);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        diag.rx_dec_in_err++;
        return r;
    }
    return -EAGAIN;
}

}  // namespace

int prepare_preview_sink(component_sink *preview, bool /*kmsdrm*/, int w, int h)
{
    return static_cast<sdl_sink *>(preview)->prepare(w, h);
}

void present_thread_main(component_sink *display, present_pdu_queue *present_queue, int width, int height,
                         bool kmsdrm, bool sdl_open_on_thread, bench_diag *diag)
{
    if (nullptr == display || nullptr == present_queue || nullptr == diag)
    {
        return;
    }
    if (sdl_open_on_thread)
    {
        if (display->open() < 0)
        {
            return;
        }
        (void)prepare_preview_sink(display, kmsdrm, width, height);
    }
    else if (kmsdrm)
    {
        (void)prepare_preview_sink(display, kmsdrm, width, height);
    }
    std::shared_ptr<pdu_wakeup> wake = std::make_shared<pdu_wakeup>();
    present_queue->bind_wakeup(wake);
    auto *sink_pdu = dynamic_cast<component_input *>(display);
    component &deadline_owner = *display;
    while (g_run.load(std::memory_order_relaxed))
    {
        component_pdu pdu;
        if (!present_queue->pop(pdu, *wake, deadline_owner))
        {
            continue;
        }
        if (nullptr == sink_pdu)
        {
            diag->rx_present_err++;
            continue;
        }
        log_pdu_stage_latency("present", pdu);
        if (sink_pdu->input(std::move(pdu)) == 0)
        {
            diag->rx_present_ok++;
        }
        else
        {
            diag->rx_present_err++;
        }
        if (auto *sdl = dynamic_cast<sdl_sink *>(display))
        {
            (void)sdl->present_pending();
        }
    }
}

void rx_net_thread_main(stream_receiver *rcv, rtp_h264_depay *depay, pdu_rx_au_queue *au_in_queue,
                        bench_diag *diag)
{
    auto *rcv_out = dynamic_cast<component_output *>(rcv);
    auto *depay_in = dynamic_cast<component_input *>(depay);
    auto *depay_out = dynamic_cast<component_output *>(depay);
    auto *rcv_owner = dynamic_cast<component *>(rcv);
    if (nullptr == rcv || nullptr == depay || nullptr == rcv_out || nullptr == depay_in ||
        nullptr == depay_out || nullptr == rcv_owner)
    {
        return;
    }
    std::shared_ptr<pdu_wakeup> wake = std::make_shared<pdu_wakeup>();
    rcv_owner->set_wakeup(wake);
    uint64_t au_seq = 0;
    bool     have_au = false;
    while (g_run.load(std::memory_order_relaxed))
    {
        component_pdu sock_pdu;
        const int     got = rcv_out->output(sock_pdu);
        if (0 != got)
        {
            if (got < 0 && -EAGAIN != got)
            {
                break;
            }
            if (-EAGAIN == got)
            {
                wait_for_pdu(*wake, *rcv_owner, g_run);
            }
            continue;
        }
        diag->rx_udp++;
        if (depay_in->input(std::move(sock_pdu)) < 0)
        {
            diag->rx_depay_err++;
            continue;
        }
        component_pdu au;
        while (g_run.load(std::memory_order_relaxed) && depay_out->output(au) == 0)
        {
            diag->rx_depay_au++;
            if (g_rx.skip_decode.load())
            {
                continue;
            }
            note_pdu_sequence_gap(au.seq, au_seq, have_au, nullptr);
            log_pdu_stage_latency("depay", au);
            au_in_queue->push(std::move(au), &diag->rx_au_q_drop);
        }
    }
}

void decode_thread_main(h264_decoder_mpp *dec, present_pdu_queue *present_queue, pdu_rx_au_queue *au_in_queue,
                        bench_diag *diag)
{
    pin_current_thread_to_cpu(g_cpu_map.rx);
    auto *owner = dynamic_cast<component *>(dec);
    if (nullptr == owner)
    {
        return;
    }
    std::shared_ptr<pdu_wakeup> wake = std::make_shared<pdu_wakeup>();
    owner->set_wakeup(wake);
    au_in_queue->bind_wakeup(wake);
    present_queue->bind_wakeup(wake);
    component_pdu pdu;
    bool          holding = false;
    uint64_t            last_seq = 0;
    bool                have_seq = false;
    while (g_run.load(std::memory_order_relaxed))
    {
        if (g_rx.skip_decode.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!holding)
        {
            if (!au_in_queue->pop(pdu, *wake, *owner))
            {
                drain_dec_pdus(dec, present_queue, *diag);
                continue;
            }
            note_pdu_sequence_gap(pdu.seq, last_seq, have_seq, nullptr);
            if (is_caps(pdu.sdu_type))
            {
                if (ensure_decoder_open(dec) < 0)
                {
                    holding = false;
                    continue;
                }
                auto *dec_pdu_in = dynamic_cast<component_input *>(dec);
                if (nullptr != dec_pdu_in)
                {
                    const int cr = dec_pdu_in->input(std::move(pdu));
                    if (0 == cr)
                    {
                        diag->rx_dec_in_ok++;
                    }
                    else if (-EAGAIN != cr && -ECANCELED != cr)
                    {
                        diag->rx_dec_in_err++;
                    }
                }
                drain_dec_pdus(dec, present_queue, *diag);
                continue;
            }
            log_pdu_stage_latency("dec_in", pdu);
            holding = true;
        }
        const int r = feed_decoder_pdu(dec, pdu, present_queue, *diag);
        if (0 == r)
        {
            holding = false;
            drain_dec_pdus(dec, present_queue, *diag);
            continue;
        }
        if (-EAGAIN == r)
        {
            drain_dec_pdus(dec, present_queue, *diag);
            continue;
        }
        if (-ECANCELED == r)
        {
            break;
        }
        holding = false;
    }
    drain_dec_pdus(dec, present_queue, *diag);
}

#endif

}  // namespace vstreamer::test_app
