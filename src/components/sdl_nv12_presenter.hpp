#ifndef VSTREAMER_COMPONENTS_SDL_NV12_PRESENTER_HPP
#define VSTREAMER_COMPONENTS_SDL_NV12_PRESENTER_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_SDL_SINK
#error "sdl_nv12_presenter requires -DENABLE_SDL_SINK=ON"
#endif

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "core/shared_sized_buffer.hpp"

namespace vstreamer
{

struct nv12_present_sample
{
    shared_sized_buffer buf;
    int                 width = 0;
    int                 height = 0;
    uint64_t            ts_us = 0;
};

/* Shared SDL NV12 window/texture path for sdl_sink. */
class sdl_nv12_presenter
{
public:
    sdl_nv12_presenter(const char *log_tag, const char *video_driver);

    int  open();
    void close();

    /* Create window/texture before first frame (black until present). */
    int prepare(int w, int h, bool &session_open);

    int present(const nv12_present_sample &f, bool &session_open);

    void set_queue_capacity(size_t cap);
    [[nodiscard]] size_t queue_capacity() const;
    [[nodiscard]] size_t queue_size() const;

    /* Non-blocking enqueue; -EAGAIN when the pending queue is full. */
    int try_enqueue(const nv12_present_sample &f);
    /* Drop oldest pending frame when full (live preview policy). */
    int enqueue_drop(const nv12_present_sample &f, bool *dropped_oldest);
    /* Present all pending frames on the calling thread. */
    int drain_pending(bool &session_open);
    void clear_pending();

    void set_title(std::string_view title);
    void stats_string(char *buf, size_t buflen, uint64_t frames_in) const;

    [[nodiscard]] int live_width() const { return live_w; }
    [[nodiscard]] int live_height() const { return live_h; }
    [[nodiscard]] std::thread::id bound_render_thread() const;

    using test_present_delay_hook_fn = void (*)(int delay_ms);
    static void set_test_present_delay_hook(test_present_delay_hook_fn hook);
    static void clear_test_present_delay_hook();

    static test_present_delay_hook_fn s_test_present_delay_hook;

private:
    void destroy_video_locked();
    void destroy_texture_locked();
    void pump_events_locked(bool &session_open);
    int  ensure_video_locked(int w, int h);
    int  present_nv12_locked(const nv12_present_sample &f, bool &session_open,
                             std::unique_lock<std::mutex> &lock);

    const char *log_tag;
    const char *video_driver;

    mutable std::mutex mu;

    bool sdl_ready = false;

    std::string title = "vstreamer";

    int live_w = 0;
    int live_h = 0;
    int tex_w = 0;
    int tex_h = 0;

    void *window = nullptr;
    void *renderer = nullptr;
    void *texture = nullptr;

    int  console_tty_fd = -1;
    int  console_vt = 0;
    bool console_kd_graphics = false;

    std::thread::id render_thread_id {};
    bool            render_thread_bound = false;

    uint64_t present_ok_count = 0;
    uint64_t present_fail_count = 0;
    double   last_latency_ms = 0.0;
    mutable std::string last_err;

    static constexpr size_t k_default_queue_depth = 1;
    size_t                         queue_cap = k_default_queue_depth;
    std::deque<nv12_present_sample> pending;

    void note_present_failure(const char *op);
    void clear_sdl_error();
    bool check_sdl_error_after(const char *op);
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_SDL_NV12_PRESENTER_HPP
