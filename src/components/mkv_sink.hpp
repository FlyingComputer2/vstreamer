#ifndef VSTREAMER_COMPONENTS_MKV_SINK_HPP
#define VSTREAMER_COMPONENTS_MKV_SINK_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_MKV_SINK
#error "mkv_sink requires -DENABLE_MKV_SINK=ON"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/component_pdu.hpp"
#include "core/component_sink.hpp"
#include "core/component_input.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"

namespace vstreamer
{

/* MJPEG → Matroska via libavformat; incremental cluster writes for live-ish playback. */
class mkv_sink : public component_sink
{
public:
    mkv_sink();
    ~mkv_sink() override;

    mkv_sink(const mkv_sink &) = delete;
    mkv_sink &operator=(const mkv_sink &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;
    int input(component_pdu &&in) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    void stop_locked();
    int  start_locked(int w, int h);
    int  ensure_session_locked(int w, int h);
    int  write_frame_locked(const component_pdu &in);
    void split_output_template_locked();
    [[nodiscard]] std::string segment_path_locked() const;

    void mux_thread_main();
    void stop_mux_thread();
    void wait_mux_idle();

    [[nodiscard]] int input_pdu_locked(component_pdu &&in);
    [[nodiscard]] bool coded_caps_acceptable(const video_coded_caps &caps) const;

    [[nodiscard]] int try_enqueue_locked(component_pdu &&pkt);
    [[nodiscard]] int enqueue_drop_locked(component_pdu &&pkt, bool *dropped_oldest);

    static const std::vector<port_desc> &input_ports();

    mutable std::mutex mu;

    bool opened = false;

    std::string output_template;
    std::string output_stem;
    std::string output_ext;
    std::string output_path;
    int         segment_index = 0;
    int         cfg_width = 0;
    int         cfg_height = 0;
    int         fps = 30;

    bool recording = false;
    int  live_w = 0;
    int  live_h = 0;
    int64_t last_mux_pts = -1;
    int64_t segment_pts_base = -1;
    double  t0 = 0.0;
    uint64_t frames_out = 0;
    uint64_t dropped = 0;

    static constexpr size_t k_default_queue_depth = 8;
    size_t                  queue_cap = k_default_queue_depth;

    bool           have_input_caps_ = false;
    bool           caps_reject_ = false;
    video_coded_caps input_caps_ {};

    std::mutex              q_mu;
    std::condition_variable q_cv;
    std::deque<component_pdu> queue;
    std::thread             mux_thread;
    std::atomic<bool>       mux_stop {false};
    std::atomic<int>        mux_in_flight {0};

    /* Opaque libav handles; typed in the .cpp. */
    void *fmt = nullptr;
    void *stream = nullptr;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_MKV_SINK_HPP
