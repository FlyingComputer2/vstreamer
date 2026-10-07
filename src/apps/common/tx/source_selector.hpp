#ifndef VSTREAMER_APPS_TX_SOURCE_SELECTOR_HPP
#define VSTREAMER_APPS_TX_SOURCE_SELECTOR_HPP

#include "core/component_pdu.hpp"
#include "core/component_source.hpp"
#include "core/pdu_wakeup.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vstreamer::apps::tx
{

enum class source_kind
{
    camera,
    noise_fallback
};

/* UVC preferred; NV12 noise at app-given size when camera is absent or returns -ENODEV. */
class source_selector
{
public:
    using on_switch_fn = std::function<void(source_kind kind, int width, int height, int fps)>;
    using push_pdu_fn = std::function<void(component_pdu &&)>;

    source_selector(component_source &camera, component_source &noise, int noise_width,
                    int noise_height, int noise_fps, on_switch_fn on_switch);

    void set_push_pdu_handler(push_pdu_fn push_pdu_in);
    void bind_source_wakeups(std::shared_ptr<pdu_wakeup> w);

    int  open();
    void close();

    /* One source-thread iteration: may block up to timeout_ms on the active source. */
    int poll_once(int timeout_ms);
    int poll_once_pdu(pdu_wakeup &w);

    [[nodiscard]] source_kind active_kind() const
    {
        return kind;
    }

    int configure(std::string_view key, std::string_view value);
    int query(std::string_view key, std::string *value) const;

private:
    bool switch_allowed() const;
    void switch_to_noise();
    void switch_to_camera(int width, int height, int fps);
    void emit_switch_caps(source_kind kind, int width, int height, int fps);
    int  poll_active_pdu(component_pdu &out, pdu_wakeup &w);
    bool read_camera_geometry(int *width, int *height, int *fps) const;

    component_source &camera;
    component_source &noise;
    int               noise_width;
    int               noise_height;
    int               noise_fps;
    on_switch_fn      on_switch;
    push_pdu_fn       push_pdu;
    std::shared_ptr<pdu_wakeup> source_wake_;

    source_kind kind = source_kind::camera;
    std::string camera_error;
    bool        logged_open_error = false;
    bool        logged_noise = false;
    bool        logged_camera = false;

    std::chrono::steady_clock::time_point last_switch_time {};
    std::chrono::steady_clock::time_point last_camera_probe {};
    bool                                    have_last_switch_time = false;
};

}  // namespace vstreamer::apps::tx

#endif  // VSTREAMER_APPS_TX_SOURCE_SELECTOR_HPP
