#include "components/mkv_sink.hpp"

#include "core/key_util.hpp"
#include "core/port_caps.hpp"
#include "core/thread_spawn.hpp"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace vstreamer
{
namespace
{

double now_sec()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int parse_size(std::string_view s, int *w, int *h)
{
    if (nullptr == w || nullptr == h || s.empty())
    {
        return -EINVAL;
    }
    char buf[64];
    if (s.size() >= sizeof(buf))
    {
        return -EINVAL;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    char *x = std::strchr(buf, 'x');
    if (nullptr == x)
    {
        x = std::strchr(buf, 'X');
    }
    if (nullptr == x || x == buf || x[1] == '\0')
    {
        return -EINVAL;
    }
    *x = '\0';
    int64_t ww = 0;
    int64_t hh = 0;
    if (key_parse_i64(buf, &ww) < 0 || key_parse_i64(x + 1, &hh) < 0)
    {
        return -EINVAL;
    }
    if (ww < 16 || hh < 16 || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

const std::vector<port_desc> &mkv_input_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_CODED;
        p.caps.push_back(caps);
        port_caps_entry data {};
        data.sdu_type = sdu_type_e::MJPEG;
        p.caps.push_back(data);
        data.sdu_type = sdu_type_e::H264_AU;
        p.caps.push_back(data);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

}  // namespace

const std::vector<port_desc> &mkv_sink::input_ports()
{
    return mkv_input_ports();
}

mkv_sink::mkv_sink() = default;

mkv_sink::~mkv_sink()
{
    close();
}

std::string mkv_sink::name() const
{
    return "mkv_sink";
}


void mkv_sink::stop_locked()
{
    if (!recording)
    {
        return;
    }

    (void)writer.close();

    output_path.clear();
    last_mux_pts = -1;
    t0 = 0.0;
    live_w = 0;
    live_h = 0;
    recording = false;
}

void mkv_sink::split_output_template_locked()
{
    output_stem.clear();
    output_ext = ".mkv";
    if (output_template.empty())
    {
        return;
    }
    const auto slash = output_template.find_last_of('/');
    const auto dot = output_template.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    {
        output_stem = output_template;
        return;
    }
    output_stem = output_template.substr(0, dot);
    output_ext = output_template.substr(dot);
}

std::string mkv_sink::segment_path_locked() const
{
    if (output_stem.empty())
    {
        return {};
    }
    char idx[16];
    std::snprintf(idx, sizeof(idx), "-%03d", segment_index);
    return output_stem + idx + output_ext;
}

int mkv_sink::start_locked(int w, int h, mkv_codec_e codec)
{
    if (output_template.empty())
    {
        return -EINVAL;
    }

    stop_locked();

    ++segment_index;
    output_path = segment_path_locked();
    if (output_path.empty())
    {
        return -EINVAL;
    }

    mkv_track_config cfg;
    cfg.codec = codec;
    cfg.width = w;
    cfg.height = h;
    cfg.fps = fps > 0 ? fps : 30;
    const int r = writer.open(output_path, cfg);
    if (r < 0)
    {
        output_path.clear();
        return r;
    }

    live_w = w;
    live_h = h;
    live_codec = codec;
    last_mux_pts = -1;
    segment_pts_base = -1;
    last_frame_ts_us = -1;
    t0 = now_sec();
    recording = true;
    return 0;
}

int mkv_sink::ensure_session_locked(int w, int h, mkv_codec_e codec)
{
    if (output_template.empty())
    {
        return 0;
    }

    if (cfg_width > 0 && cfg_height > 0)
    {
        w = cfg_width;
        h = cfg_height;
    }

    if (w <= 0 || h <= 0)
    {
        return -EINVAL;
    }

    if (recording && (w != live_w || h != live_h || codec != live_codec))
    {
        stop_locked();
    }

    if (!recording)
    {
        return start_locked(w, h, codec);
    }
    return 0;
}

int mkv_sink::write_frame_locked(const component_pdu &in)
{
    const int     use_fps = fps > 0 ? fps : 30;
    const int64_t frame_ts_us = static_cast<int64_t>(in.ts_us);
    const int64_t pts_base = segment_pts_base < 0 ? frame_ts_us : segment_pts_base;
    int64_t       mux_pts = frame_ts_us - pts_base;
    if (mux_pts < 0)
    {
        mux_pts = 0;
    }
    int64_t duration_us = 1000000 / use_fps;
    if (last_frame_ts_us >= 0)
    {
        const int64_t delta = frame_ts_us - last_frame_ts_us;
        if (delta > 0)
        {
            duration_us = delta;
        }
    }
    if (last_mux_pts >= 0 && mux_pts <= last_mux_pts)
    {
        mux_pts = last_mux_pts + 1;
    }

    const int r = writer.write_frame(in.sdu.u8(), in.sdu.size(), mux_pts, duration_us,
                                     has_flag(in, pdu_flag_e::KEY));
    if (r < 0)
    {
        return r;
    }
    if (1 == r)
    {
        /* H.264 before the first keyframe with SPS/PPS: not decodable, not timed. */
        ++skipped;
        return 0;
    }

    segment_pts_base = pts_base;
    last_frame_ts_us = frame_ts_us;
    last_mux_pts = mux_pts;
    ++frames_out;
    return 0;
}

bool mkv_sink::coded_caps_acceptable(const video_coded_caps &caps) const
{
    if (caps.width <= 0 || caps.height <= 0)
    {
        return false;
    }
    for (const port_caps_entry &entry : input_ports()[0].caps)
    {
        if (entry.sdu_type == sdu_type_e::CAPS_VIDEO_CODED && match(entry, caps))
        {
            return true;
        }
    }
    return false;
}

int mkv_sink::try_enqueue_locked(component_pdu &&pkt)
{
    if (queue.size() >= queue_cap)
    {
        return -EAGAIN;
    }
    queue.push_back(std::move(pkt));
    return 0;
}

int mkv_sink::enqueue_drop_locked(component_pdu &&pkt, bool *dropped_oldest)
{
    if (nullptr != dropped_oldest)
    {
        *dropped_oldest = false;
    }
    if (queue.size() >= queue_cap && !queue.empty())
    {
        queue.pop_front();
        if (nullptr != dropped_oldest)
        {
            *dropped_oldest = true;
        }
    }
    queue.push_back(std::move(pkt));
    return 0;
}

void mkv_sink::mux_thread_main()
{
    while (true)
    {
        component_pdu pkt;
        {
            std::unique_lock<std::mutex> lock(q_mu);
            q_cv.wait(lock, [this] {
                return !queue.empty() || mux_stop.load(std::memory_order_relaxed);
            });
            if (queue.empty())
            {
                if (mux_stop.load(std::memory_order_relaxed))
                {
                    break;
                }
                continue;
            }
            pkt = std::move(queue.front());
            queue.pop_front();
            /* Counted under q_mu with the pop, so wait_mux_idle never sees an empty queue
             * and nothing in flight while this packet is in hand. */
            mux_in_flight.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(mu);
            if (opened)
            {
                int w = input_caps_.width;
                int h = input_caps_.height;
                const mkv_codec_e codec = pkt.sdu_type == sdu_type_e::H264_AU
                                              ? mkv_codec_e::H264
                                              : mkv_codec_e::MJPEG;
                const int sr = ensure_session_locked(w, h, codec);
                if (sr >= 0 && recording && write_frame_locked(pkt) < 0)
                {
                    /* mkv_writer stops at its first I/O error: close what was written and
                     * let the next frame start a new segment. */
                    ++write_errors;
                    stop_locked();
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(q_mu);
            mux_in_flight.fetch_sub(1, std::memory_order_relaxed);
        }
        q_cv.notify_all();
    }
}

void mkv_sink::wait_mux_idle()
{
    std::unique_lock<std::mutex> lock(q_mu);
    q_cv.wait(lock, [this] {
        return queue.empty() && mux_in_flight.load(std::memory_order_relaxed) == 0;
    });
}

void mkv_sink::stop_mux_thread()
{
    wait_mux_idle();
    {
        /* Under q_mu: set outside it, the store and notify can land between the mux thread's
         * predicate check and its sleep, and the thread never wakes to see it. */
        std::lock_guard<std::mutex> lock(q_mu);
        mux_stop.store(true, std::memory_order_relaxed);
    }
    q_cv.notify_all();
    if (mux_thread.joinable())
    {
        mux_thread.join();
    }
    mux_stop.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(q_mu);
        queue.clear();
    }
}

int mkv_sink::input_pdu_locked(component_pdu &&in)
{
    if (0 != in.port)
    {
        return -EINVAL;
    }
    if (in.sdu_type == sdu_type_e::CAPS_VIDEO_CODED)
    {
        video_coded_caps caps {};
        if (read_caps(in, &caps) != 0)
        {
            return -EINVAL;
        }
        if (!coded_caps_acceptable(caps))
        {
            caps_reject_ = true;
            have_input_caps_ = false;
            return -ENOTSUP;
        }
        caps_reject_ = false;
        input_caps_ = caps;
        have_input_caps_ = true;
        return 0;
    }
    if (in.sdu_type != sdu_type_e::MJPEG && in.sdu_type != sdu_type_e::H264_AU)
    {
        return -EINVAL;
    }
    if (caps_reject_ || !have_input_caps_)
    {
        return -ENOTSUP;
    }
    if (output_template.empty())
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(q_mu);
    const int                   r = try_enqueue_locked(std::move(in));
    if (0 == r)
    {
        q_cv.notify_one();
    }
    return r;
}

int mkv_sink::input(component_pdu &&in)
{
    if (0 == in.port && in.sdu_type == sdu_type_e::CAPS_VIDEO_CODED)
    {
        video_coded_caps caps {};
        if (read_caps(in, &caps) != 0)
        {
            return -EINVAL;
        }
        bool resize = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!opened)
            {
                return -EBADF;
            }
            if (!coded_caps_acceptable(caps))
            {
                caps_reject_ = true;
                have_input_caps_ = false;
                return -ENOTSUP;
            }
            resize = have_input_caps_ && (input_caps_.width != caps.width || input_caps_.height != caps.height);
        }
        if (resize)
        {
            wait_mux_idle();
            std::lock_guard<std::mutex> lock(mu);
            if (!opened)
            {
                return -EBADF;
            }
            stop_locked();
            caps_reject_ = false;
            input_caps_ = caps;
            have_input_caps_ = true;
            return 0;
        }
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }
    return input_pdu_locked(std::move(in));
}

int mkv_sink::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    opened = true;
    mux_stop.store(false, std::memory_order_relaxed);
    /* Runs mkv_writer (stdio, FATFS on ESP-IDF). */
    mux_thread = spawn_thread(mux_place.opts("vs_mkv", 8 * 1024), [this] { mux_thread_main(); });
    return 0;
}

void mkv_sink::close()
{
    stop_mux_thread();
    std::lock_guard<std::mutex> lock(mu);
    stop_locked();
    opened = false;
    have_input_caps_ = false;
    caps_reject_ = false;
}


int mkv_sink::configure(std::string_view key, std::string_view value)
{
    if (const int pr = thread_place_configure(&mux_place, "mux", key, value); -ENOTSUP != pr)
    {
        return pr;
    }
    std::string_view v = value;

    std::lock_guard<std::mutex> lock(mu);

    if (key == "output")
    {
        if (v.empty())
        {
            stop_locked();
            output_template.clear();
            output_stem.clear();
            output_ext.clear();
            segment_index = 0;
            return 0;
        }
        if (v.size() >= 4096)
        {
            return -EINVAL;
        }
        const std::string next(v);
        if (output_template != next)
        {
            stop_locked();
            output_template = next;
            segment_index = 0;
            split_output_template_locked();
        }
        return 0;
    }

    if (key == "size")
    {
        int w = 0;
        int h = 0;
        int r = parse_size(v, &w, &h);
        if (r < 0)
        {
            return r;
        }
        cfg_width = w;
        cfg_height = h;
        if (recording && (w != live_w || h != live_h))
        {
            stop_locked();
        }
        return 0;
    }

    if (key == "fps")
    {
        int64_t n = 0;
        std::string tmp(v);
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n <= 0 || n > 240)
        {
            return -EINVAL;
        }
        /* A restart costs H.264 everything up to the next IDR: only on a real change. */
        if (static_cast<int>(n) != fps)
        {
            fps = static_cast<int>(n);
            if (recording)
            {
                stop_locked();
            }
        }
        return 0;
    }

    if (key == "queue_depth")
    {
        int64_t n = 0;
        std::string tmp(v);
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n <= 0 || n > 4096)
        {
            return -EINVAL;
        }
        queue_cap = static_cast<size_t>(n);
        return 0;
    }

    return -ENOTSUP;
}

int mkv_sink::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    if (const int pr = thread_place_query(mux_place, "mux", key, value); -ENOTSUP != pr)
    {
        return pr;
    }

    int r = port_caps_query(input_ports(), true, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }

    std::lock_guard<std::mutex> lock(mu);

    if (key == "output")
    {
        *value = output_template;
        return 0;
    }

    if (key == "segment")
    {
        *value = output_path;
        return 0;
    }

    if (key == "stats")
    {
        if (recording)
        {
            double elapsed = now_sec() - t0;
            if (elapsed < 0.0)
            {
                elapsed = 0.0;
            }
            const int mins = static_cast<int>(elapsed) / 60;
            const int secs = static_cast<int>(elapsed) % 60;
            char      buf[64];
            std::snprintf(buf, sizeof(buf), "recording %d:%02d frames=%" PRIu64, mins, secs,
                          frames_out);
            *value = buf;
        }
        else
        {
            *value = "idle";
        }
        return 0;
    }

    if (key == "write_errors")
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, write_errors);
        *value = buf;
        return 0;
    }

    if (key == "skipped")
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, skipped);
        *value = buf;
        return 0;
    }

    if (key == "dropped")
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, dropped);
        *value = buf;
        return 0;
    }

    return -ENOTSUP;
}

}  // namespace vstreamer
