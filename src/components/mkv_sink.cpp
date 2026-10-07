#include "components/mkv_sink.hpp"

#include "core/key_util.hpp"
#include "core/port_caps.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

namespace vstreamer
{
namespace
{

double now_sec()
{
    return static_cast<double>(av_gettime()) / 1000000.0;
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

    auto *oc = static_cast<AVFormatContext *>(fmt);
    if (nullptr != oc)
    {
        av_write_trailer(oc);
        if (!(oc->oformat->flags & AVFMT_NOFILE))
        {
            avio_closep(&oc->pb);
        }
        avformat_free_context(oc);
    }

    fmt = nullptr;
    stream = nullptr;
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
    char buf[4096];
    std::snprintf(buf, sizeof(buf), "%s-%03d%s", output_stem.c_str(), segment_index, output_ext.c_str());
    return buf;
}

int mkv_sink::start_locked(int w, int h)
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

    AVFormatContext *oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "matroska", output_path.c_str()) < 0 || nullptr == oc)
    {
        return -EIO;
    }

    AVStream *st = avformat_new_stream(oc, nullptr);
    if (nullptr == st)
    {
        avformat_free_context(oc);
        return -EIO;
    }

    const int use_fps = fps > 0 ? fps : 30;
    st->time_base = AVRational{1, 1000000};
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_MJPEG;
    st->codecpar->width = w;
    st->codecpar->height = h;
    st->codecpar->format = AV_PIX_FMT_YUVJ420P;

    if (!(oc->oformat->flags & AVFMT_NOFILE))
    {
        if (avio_open(&oc->pb, output_path.c_str(), AVIO_FLAG_WRITE) < 0)
        {
            avformat_free_context(oc);
            return -EIO;
        }
    }

    AVDictionary *mux_opts = nullptr;
    av_dict_set(&mux_opts, "live", "1", 0);
    if (avformat_write_header(oc, &mux_opts) < 0)
    {
        av_dict_free(&mux_opts);
        if (!(oc->oformat->flags & AVFMT_NOFILE))
        {
            avio_closep(&oc->pb);
        }
        avformat_free_context(oc);
        return -EIO;
    }
    av_dict_free(&mux_opts);

    fmt = oc;
    stream = st;
    live_w = w;
    live_h = h;
    last_mux_pts = -1;
    segment_pts_base = -1;
    last_frame_ts_us = -1;
    t0 = now_sec();
    recording = true;
    return 0;
}

int mkv_sink::ensure_session_locked(int w, int h)
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

    if (recording && (w != live_w || h != live_h))
    {
        stop_locked();
    }

    if (!recording)
    {
        return start_locked(w, h);
    }
    return 0;
}

int mkv_sink::write_frame_locked(const component_pdu &in)
{
    auto *oc = static_cast<AVFormatContext *>(fmt);
    auto *st = static_cast<AVStream *>(stream);

    AVPacket *pkt = av_packet_alloc();
    if (nullptr == pkt)
    {
        return -ENOMEM;
    }

    uint8_t *buf = static_cast<uint8_t *>(av_malloc(in.sdu.size()));
    if (nullptr == buf)
    {
        av_packet_free(&pkt);
        return -ENOMEM;
    }
    std::memcpy(buf, in.sdu.u8(), in.sdu.size());

    pkt->data = buf;
    pkt->size = static_cast<int>(in.sdu.size());

    const int use_fps = fps > 0 ? fps : 30;
    const int64_t frame_ts_us = static_cast<int64_t>(in.ts_us);
    if (segment_pts_base < 0)
    {
        segment_pts_base = frame_ts_us;
    }
    int64_t mux_pts = frame_ts_us - segment_pts_base;
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
    last_frame_ts_us = frame_ts_us;
    if (last_mux_pts >= 0 && mux_pts <= last_mux_pts)
    {
        mux_pts = last_mux_pts + 1;
    }
    last_mux_pts = mux_pts;
    pkt->pts = mux_pts;
    pkt->dts = mux_pts;
    pkt->duration = duration_us;
    pkt->stream_index = st->index;
    pkt->flags |= AV_PKT_FLAG_KEY;
    pkt->buf = av_buffer_create(buf, in.sdu.size(), av_buffer_default_free, nullptr, 0);
    if (nullptr == pkt->buf)
    {
        av_free(buf);
        av_packet_free(&pkt);
        return -ENOMEM;
    }

    av_packet_rescale_ts(pkt, AVRational{1, 1000000}, st->time_base);

    int ret = av_interleaved_write_frame(oc, pkt);
    av_packet_free(&pkt);
    if (ret < 0)
    {
        return -EIO;
    }

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
        }

        mux_in_flight.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mu);
            if (opened)
            {
                int w = input_caps_.width;
                int h = input_caps_.height;
                const int sr = ensure_session_locked(w, h);
                if (sr >= 0 && recording)
                {
                    (void)write_frame_locked(pkt);
                }
            }
        }
        mux_in_flight.fetch_sub(1, std::memory_order_relaxed);
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
    mux_stop.store(true, std::memory_order_relaxed);
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
        if (!output_template.empty())
        {
            (void)ensure_session_locked(caps.width, caps.height);
        }
        return 0;
    }
    if (in.sdu_type != sdu_type_e::MJPEG)
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
    mux_thread = std::thread([this] { mux_thread_main(); });
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
        fps = static_cast<int>(n);
        if (recording)
        {
            stop_locked();
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
