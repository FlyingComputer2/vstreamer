#include "components/sdl_sink.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace vstreamer
{

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
    return 0;
}

void sdl_sink::close()
{
    std::lock_guard<std::mutex> lock(mu);
    if (present.has_value())
    {
        present->close();
        present.reset();
    }
    opened = false;
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

    const int r = present->present(f, opened);
    if (0 == r)
    {
        frames_in++;
    }
    else
    {
        const uint64_t n = frames_in + 1;
        if (n <= 12 || (n % 120) == 0)
        {
            char detail[192];
            present->stats_string(detail, sizeof(detail), frames_in);
            std::fprintf(stderr, "sdl_sink: present failed (%d", r);
            if (-r > 0 && -r < 4096)
            {
                std::fprintf(stderr, "; %s", std::strerror(-r));
            }
            std::fprintf(stderr, "; %s)\n", detail);
        }
    }
    return r;
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
    if ("video_driver" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = video_driver;
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
    if ("latency_ms" == key || "node_latency_ms" == key)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!present.has_value())
        {
            return -ENOTSUP;
        }
        char buf[32];
        const double ms = ("latency_ms" == key) ? present->last_latency_ms_value()
                                                : present->last_node_latency_ms_value();
        if (std::snprintf(buf, sizeof(buf), "%.2f", ms) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
