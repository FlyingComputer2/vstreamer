#include "components/sdl_sink.hpp"

#include "core/key_util.hpp"
#include "core/port_caps.hpp"

#include <chrono>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace vstreamer
{
namespace
{

const std::vector<port_desc> &sdl_input_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_RAW;
        p.caps.push_back(caps);
        port_caps_entry data {};
        data.sdu_type = sdu_type_e::NV12;
        p.caps.push_back(data);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

}  // namespace

const std::vector<port_desc> &sdl_sink::input_ports()
{
    return sdl_input_ports();
}

sdl_sink::sdl_sink() = default;

sdl_sink::~sdl_sink()
{
    close();
}

std::string sdl_sink::name() const
{
    return "sdl_sink";
}

media_kind_e sdl_sink::input_kind() const
{
    return media_kind_e::NV12;
}

const char *sdl_sink::presenter_video_driver() const
{
    if (video_driver == "auto")
    {
        return nullptr;
    }
    return video_driver.c_str();
}

bool sdl_sink::raw_caps_acceptable(const video_raw_caps &caps) const
{
    if (caps.width <= 0 || caps.height <= 0)
    {
        return false;
    }
    for (const port_caps_entry &entry : input_ports()[0].caps)
    {
        if (entry.sdu_type == sdu_type_e::CAPS_VIDEO_RAW && match(entry, caps))
        {
            return true;
        }
    }
    return false;
}

frame_data sdl_sink::frame_from_pdu(const component_pdu &in) const
{
    frame_data f;
    f.kind = media_kind_e::NV12;
    f.width = input_caps_.width;
    f.height = input_caps_.height;
    f.pts = static_cast<int64_t>(in.ts_us);
    f.capture_mono_ns = static_cast<int64_t>(in.ts_us) * 1000LL;
    f.key = has_flag(in, pdu_flag_e::KEY);
    f.buf = in.sdu;
    return f;
}

int sdl_sink::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    if (!present.has_value())
    {
        present.emplace("sdl_sink", presenter_video_driver());
    }
    int r = present->open();
    if (r < 0)
    {
        std::fprintf(stderr, "sdl_sink: open failed (%d", r);
        if (-r > 0 && -r < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-r));
        }
        std::fprintf(stderr, ")\n");
        present.reset();
        return r;
    }
    opened = true;
    present_stop.store(false, std::memory_order_relaxed);
    present_thread = std::thread([this] { present_thread_main(); });
    return 0;
}

void sdl_sink::stop_present_thread()
{
    present_stop.store(true, std::memory_order_relaxed);
    present_cv.notify_all();
    if (present_thread.joinable())
    {
        present_thread.join();
    }
    present_stop.store(false, std::memory_order_relaxed);
}

void sdl_sink::present_thread_main()
{
    while (!present_stop.load(std::memory_order_relaxed))
    {
        {
            std::unique_lock<std::mutex> lock(mu);
            present_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return present_stop.load(std::memory_order_relaxed) ||
                       (present.has_value() && present->queue_size() > 0);
            });
            if (present_stop.load(std::memory_order_relaxed))
            {
                break;
            }
            if (!present.has_value() || !opened)
            {
                continue;
            }
            const int r = present->drain_pending(opened);
            if (r > 0)
            {
                frames_in += static_cast<uint64_t>(r);
            }
        }
    }
    if (present.has_value())
    {
        std::lock_guard<std::mutex> lock(mu);
        while (present->queue_size() > 0 && opened)
        {
            const int tail_r = present->drain_pending(opened);
            if (tail_r > 0)
            {
                frames_in += static_cast<uint64_t>(tail_r);
            }
            else
            {
                break;
            }
        }
    }
}

void sdl_sink::close()
{
    stop_present_thread();
    std::lock_guard<std::mutex> lock(mu);
    if (present.has_value())
    {
        present->close();
        present.reset();
    }
    opened = false;
    have_input_caps_ = false;
    caps_reject_ = false;
}

int sdl_sink::prepare(int width, int height)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened || !present.has_value())
    {
        return -EBADF;
    }
    const int r = present->prepare(width, height, opened);
    if (r < 0)
    {
        std::fprintf(stderr, "sdl_sink: prepare(%d,%d) failed (%d", width, height, r);
        if (-r > 0 && -r < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-r));
        }
        std::fprintf(stderr, ")\n");
    }
    return r;
}

int sdl_sink::input_pdu_locked(component_pdu &&in)
{
    if (0 != in.port)
    {
        return -EINVAL;
    }
    if (in.sdu_type == sdu_type_e::CAPS_VIDEO_RAW)
    {
        video_raw_caps caps {};
        if (read_caps(in, &caps) != 0)
        {
            return -EINVAL;
        }
        if (!raw_caps_acceptable(caps))
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
    if (in.sdu_type != sdu_type_e::NV12)
    {
        return -EINVAL;
    }
    if (caps_reject_ || !have_input_caps_)
    {
        return -ENOTSUP;
    }
    if (!present.has_value())
    {
        return -EBADF;
    }
    const frame_data f = frame_from_pdu(in);
    const int enq = present->try_enqueue(f);
    if (0 == enq)
    {
        present_cv.notify_one();
    }
    return enq;
}

int sdl_sink::input(component_pdu &&in)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }
    return input_pdu_locked(std::move(in));
}

int sdl_sink::input(uint8_t port, const data_packet &in)
{
    if (0 != port)
    {
        return -EINVAL;
    }

    const frame_data &f = data_packet::cast<frame_data>(in);
    if (f.kind != media_kind_e::NV12)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (!opened || !present.has_value())
    {
        return -EBADF;
    }

    video_raw_caps caps {};
    caps.width = f.width > 0 ? f.width : input_caps_.width;
    caps.height = f.height > 0 ? f.height : input_caps_.height;
    if (caps.width <= 0 || caps.height <= 0)
    {
        return -EINVAL;
    }
    component_pdu caps_pdu = make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0);
    const int     cr = input_pdu_locked(std::move(caps_pdu));
    if (cr < 0)
    {
        return cr;
    }

    component_pdu pdu;
    pdu.ts_us = f.capture_mono_ns > 0 ? static_cast<uint64_t>(f.capture_mono_ns / 1000LL)
                                      : static_cast<uint64_t>(f.pts);
    pdu.sdu_type = sdu_type_e::NV12;
    pdu.port = 0;
    pdu.sdu = f.buf;
    if (f.key)
    {
        pdu.flags |= static_cast<uint8_t>(pdu_flag_e::KEY);
    }

    bool dropped_oldest = false;
    const frame_data ff = frame_from_pdu(pdu);
    const int enq = present->enqueue_drop(ff, &dropped_oldest);
    if (dropped_oldest)
    {
        dropped++;
    }
    if (0 == enq)
    {
        present_cv.notify_one();
    }
    return enq;
}

int sdl_sink::configure(std::string_view key, std::string_view value)
{
    if ("title" == key)
    {
        if (present.has_value())
        {
            present->set_title(value);
        }
        return 0;
    }
    if ("queue_depth" == key)
    {
        int64_t n = 0;
        std::string tmp(value);
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n <= 0 || n > 256)
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(mu);
        if (present.has_value())
        {
            present->set_queue_capacity(static_cast<size_t>(n));
        }
        return 0;
    }
    if ("video_driver" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened)
        {
            return -EBUSY;
        }
        if (value.empty() || value == "auto")
        {
            video_driver = "auto";
        }
        else
        {
            video_driver = std::string(value);
        }
        return 0;
    }
    return -ENOTSUP;
}

int sdl_sink::query(std::string_view key, std::string *value) const
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

    if ("video_driver" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = video_driver;
        return 0;
    }
    if ("dropped" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%" PRIu64, dropped);
        *value = buf;
        return 0;
    }
    if ("stats" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        char buf[64];
        if (present.has_value())
        {
            present->stats_string(buf, sizeof(buf), frames_in);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "frames=%" PRIu64, frames_in);
        }
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
