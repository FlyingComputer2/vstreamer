#include "apps/common/tx/source_selector.hpp"

#include "apps/common/pdu_stage.hpp"
#include "apps/common/pipeline_state.hpp"

#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/time_util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

namespace vstreamer::apps::tx
{
namespace
{

constexpr int k_camera_probe_interval_ms = 500;
constexpr int k_min_switch_interval_ms = 500;

bool key_is_noise_config(std::string_view key)
{
    return key == "noise-bandwidth" || key == "noise-randomness" || key == "noise-block-size" ||
           key == "pregenerate-frames" || key == "pregenerate-frame" ||
           key == "pregenerate_frames" || key == "pregenerate_frame";
}

bool key_is_noise_query(std::string_view key)
{
    return key_is_noise_config(key) || key == "noise-luma-block-size" || key == "noise-fft-simd" ||
           key == "noise-fft-grid";
}

int pdu_output_from_source(component_source &src, component_pdu &out)
{
    auto *po = dynamic_cast<component_output *>(&src);
    if (nullptr == po)
    {
        return -ENOTSUP;
    }
    return po->output(out);
}

}  // namespace

source_selector::source_selector(component_source &camera_in, component_source &noise_in,
                               int noise_width_in, int noise_height_in, int noise_fps_in,
                               on_switch_fn on_switch_in)
    : camera(camera_in),
      noise(noise_in),
      noise_width(noise_width_in),
      noise_height(noise_height_in),
      noise_fps(noise_fps_in),
      on_switch(std::move(on_switch_in))
{
}

void source_selector::set_push_pdu_handler(push_pdu_fn push_pdu_in)
{
    push_pdu = std::move(push_pdu_in);
}

void source_selector::emit_switch_caps(source_kind switch_kind, int width, int height, int fps)
{
    if (!push_pdu)
    {
        return;
    }
    const uint64_t ts_us = static_cast<uint64_t>(steady_mono_ns() / 1000LL);
    if (source_kind::noise_fallback == switch_kind)
    {
        video_raw_caps caps {};
        caps.width = width;
        caps.height = height;
        caps.hor_stride = width;
        caps.ver_stride = height;
        caps.fps_num = fps > 0 ? fps : 30;
        caps.fps_den = 1;
        push_pdu(make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, ts_us, 0));
        return;
    }
    video_coded_caps caps {};
    caps.width = width;
    caps.height = height;
    caps.fps_num = fps > 0 ? fps : 30;
    caps.fps_den = 1;
    push_pdu(make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, caps, ts_us, 0));
}

bool source_selector::switch_allowed() const
{
    if (!have_last_switch_time)
    {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_switch_time).count();
    return ms >= k_min_switch_interval_ms;
}

void source_selector::switch_to_noise()
{
    if (source_kind::noise_fallback == kind)
    {
        return;
    }
    if (!switch_allowed())
    {
        return;
    }
    kind = source_kind::noise_fallback;
    last_switch_time = std::chrono::steady_clock::now();
    have_last_switch_time = true;
    if (on_switch)
    {
        on_switch(source_kind::noise_fallback, noise_width, noise_height, noise_fps);
    }
    if (!logged_noise)
    {
        std::fprintf(stderr, "source: camera unavailable; NV12 noise fallback\n");
        logged_noise = true;
        logged_camera = false;
    }
    emit_switch_caps(source_kind::noise_fallback, noise_width, noise_height, noise_fps);
}

void source_selector::switch_to_camera(int width, int height, int fps)
{
    if (source_kind::camera == kind)
    {
        return;
    }
    if (!switch_allowed())
    {
        return;
    }
    kind = source_kind::camera;
    last_switch_time = std::chrono::steady_clock::now();
    have_last_switch_time = true;
    if (on_switch)
    {
        on_switch(source_kind::camera, width, height, fps);
    }
    if (!logged_camera)
    {
        std::fprintf(stderr, "source: camera capture restored\n");
        logged_camera = true;
        logged_noise = false;
    }
    emit_switch_caps(source_kind::camera, width, height, fps);
}

bool source_selector::read_camera_geometry(int *width, int *height, int *fps) const
{
    if (nullptr == width || nullptr == height || nullptr == fps)
    {
        return false;
    }
    std::string w;
    std::string h;
    std::string f;
    if (camera.query("width", &w) != 0 || camera.query("height", &h) != 0 ||
        camera.query("fps", &f) != 0)
    {
        return false;
    }
    char       *end = nullptr;
    const long  wi = std::strtol(w.c_str(), &end, 10);
    if (end == w.c_str())
    {
        return false;
    }
    const long he = std::strtol(h.c_str(), &end, 10);
    if (end == h.c_str())
    {
        return false;
    }
    const long fp = std::strtol(f.c_str(), &end, 10);
    if (end == f.c_str())
    {
        return false;
    }
    *width = static_cast<int>(wi);
    *height = static_cast<int>(he);
    *fps = static_cast<int>(fp);
    return true;
}

int source_selector::open()
{
    logged_open_error = false;
    logged_noise = false;
    logged_camera = false;
    have_last_switch_time = false;
    kind = source_kind::camera;
    camera_error.clear();

    const int nr = noise.open();
    if (nr < 0)
    {
        return nr;
    }

    const int cr = camera.open();
    if (cr < 0)
    {
        if (-cr > 0 && -cr < 4096)
        {
            camera_error = std::strerror(-cr);
        }
        else
        {
            camera_error = "camera open failed";
        }
        if (!logged_open_error)
        {
            std::fprintf(stderr, "source: camera open failed (%d; %s)\n", cr, camera_error.c_str());
            logged_open_error = true;
        }
        switch_to_noise();
        return 0;
    }
    return 0;
}

void source_selector::close()
{
    camera.close();
    noise.close();
    kind = source_kind::camera;
    logged_noise = false;
    logged_camera = false;
}

void source_selector::bind_source_wakeups(std::shared_ptr<pdu_wakeup> w)
{
    dynamic_cast<component &>(camera).set_wakeup(w);
    dynamic_cast<component &>(noise).set_wakeup(w);
}

int source_selector::poll_active_pdu(component_pdu &out, pdu_wakeup &w)
{
    component_source &active = source_kind::camera == kind ? camera : noise;
    component        &owner = dynamic_cast<component &>(active);
    while (apps::g_run.load(std::memory_order_relaxed))
    {
        const int r = pdu_output_from_source(active, out);
        if (-EAGAIN != r)
        {
            if (0 == r && push_pdu)
            {
                push_pdu(std::move(out));
            }
            return r;
        }
        apps::wait_for_pdu(w, owner, apps::g_run);
    }
    return -ECANCELED;
}

int source_selector::poll_once_pdu(pdu_wakeup &w)
{
    if (!push_pdu)
    {
        return poll_once(-1);
    }
    if (source_kind::camera == kind)
    {
        component_pdu out;
        const int     cam = pdu_output_from_source(camera, out);
        if (0 == cam)
        {
            push_pdu(std::move(out));
            return 0;
        }
        if (-ENODEV != cam)
        {
            if (-EAGAIN == cam)
            {
                apps::wait_for_pdu(w, dynamic_cast<component &>(camera), apps::g_run);
                return -EAGAIN;
            }
            return cam;
        }
        switch_to_noise();
        if (source_kind::camera == kind)
        {
            return -EAGAIN;
        }
        component_pdu noise_out;
        return poll_active_pdu(noise_out, w);
    }
    const auto now = std::chrono::steady_clock::now();
    const auto since_probe =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_camera_probe).count();
    if (since_probe >= k_camera_probe_interval_ms)
    {
        last_camera_probe = now;
        component_pdu probe;
        const int     cam = pdu_output_from_source(camera, probe);
        if (0 == cam)
        {
            int width = noise_width;
            int h = noise_height;
            int f = noise_fps;
            (void)read_camera_geometry(&width, &h, &f);
            switch_to_camera(width, h, f);
            if (source_kind::camera == kind)
            {
                push_pdu(std::move(probe));
                return 0;
            }
        }
    }
    component_pdu noise_out;
    return poll_active_pdu(noise_out, w);
}

int source_selector::poll_once(int timeout_ms)
{
    pdu_wakeup local_wake;
    if (timeout_ms < 0)
    {
        return poll_once_pdu(local_wake);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int r = poll_once_pdu(local_wake);
        if (0 == r || -EAGAIN != r)
        {
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return -EAGAIN;
}

int source_selector::configure(std::string_view key, std::string_view value)
{
    if (key_is_noise_config(key))
    {
        return noise.configure(key, value);
    }
    return camera.configure(key, value);
}

int source_selector::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    if (key == "state")
    {
        *value = (source_kind::noise_fallback == kind) ? "noise_fallback" : "camera";
        return 0;
    }
    if (key == "camera_error")
    {
        *value = camera_error;
        return 0;
    }
    if (key_is_noise_query(key))
    {
        return noise.query(key, value);
    }
    if (source_kind::noise_fallback == kind && (key == "media_type" || key == "pixel_type"))
    {
        return noise.query(key, value);
    }
    return camera.query(key, value);
}

}  // namespace vstreamer::apps::tx
