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

nv12_present_sample sdl_sink::sample_from_pdu(const component_pdu &in) const
{
    nv12_present_sample s;
    s.width = input_caps_.width;
    s.height = input_caps_.height;
    s.ts_us = in.ts_us;
    s.buf = in.sdu;
    return s;
}

int sdl_sink::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened.load(std::memory_order_relaxed))
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
    opened.store(true, std::memory_order_release);
    return 0;
}

void sdl_sink::close()
{
    close_requested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mu);
        if (present.has_value())
        {
            present->clear_pending();
        }
    }
    std::lock_guard<std::mutex> lock(mu);
    if (present.has_value())
    {
        present->close();
        present.reset();
    }
    opened.store(false, std::memory_order_release);
    close_requested.store(false, std::memory_order_release);
    have_input_caps_ = false;
    caps_reject_ = false;
}

int sdl_sink::prepare(int width, int height)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened.load(std::memory_order_acquire) || !present.has_value())
    {
        return -EBADF;
    }
    prepared_w_ = width;
    prepared_h_ = height;
    bool session_open = opened.load(std::memory_order_relaxed);
    const int r = present->prepare(width, height, session_open);
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

std::thread::id sdl_sink::bound_render_thread() const
{
    std::lock_guard<std::mutex> lock(mu);
    if (!present.has_value())
    {
        return {};
    }
    return present->bound_render_thread();
}

int sdl_sink::present_pending()
{
    bool session_open = false;
    sdl_nv12_presenter *presenter = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (close_requested.load(std::memory_order_acquire) ||
            !opened.load(std::memory_order_acquire) || !present.has_value())
        {
            return -EBADF;
        }
        session_open = true;
        presenter = &(*present);
    }
    const int r = presenter->drain_pending(session_open);
    if (r > 0)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (opened.load(std::memory_order_relaxed))
        {
            frames_in += static_cast<uint64_t>(r);
        }
        return 0;
    }
    if (r < 0)
    {
        return r;
    }
    return -EAGAIN;
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
    if (!have_input_caps_ && prepared_w_ > 0 && prepared_h_ > 0)
    {
        input_caps_.width = prepared_w_;
        input_caps_.height = prepared_h_;
        input_caps_.hor_stride = prepared_w_;
        input_caps_.ver_stride = prepared_h_;
        have_input_caps_ = true;
        caps_reject_ = false;
    }
    if (caps_reject_ || !have_input_caps_)
    {
        return -ENOTSUP;
    }
    if (!present.has_value())
    {
        return -EBADF;
    }
    const nv12_present_sample f = sample_from_pdu(in);
    const int enq = present->try_enqueue(f);
    if (-EAGAIN == enq)
    {
        dropped++;
    }
    return enq;
}

int sdl_sink::input(component_pdu &&in)
{
    std::lock_guard<std::mutex> lock(mu);
    if (!opened.load(std::memory_order_acquire))
    {
        return -EBADF;
    }
    return input_pdu_locked(std::move(in));
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
        if (opened.load(std::memory_order_acquire))
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
