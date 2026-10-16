#include "components/esp32_h264_isp_src.hpp"

#include "components/esp32p4_csi_camera_src.hpp"
#include "components/h264_encoder_esp32p4.hpp"

#include "core/key_util.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/thread_spawn.hpp"

#include <chrono>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <thread>
#include <utility>

namespace vstreamer
{
namespace
{

constexpr size_t k_pipeline_stack = 24 * 1024;

}  // namespace

const std::vector<port_desc> &esp32_h264_isp_src::output_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc       p;
        port_caps_entry coded {};
        coded.sdu_type = sdu_type_e::CAPS_VIDEO_CODED;
        p.caps.push_back(coded);
        port_caps_entry au {};
        au.sdu_type = sdu_type_e::H264_AU;
        p.caps.push_back(au);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

esp32_h264_isp_src::esp32_h264_isp_src()
    : cam_(std::make_unique<esp32p4_csi_camera_src>())
    , enc_(std::make_unique<h264_encoder_esp32p4>())
{
}

esp32_h264_isp_src::~esp32_h264_isp_src()
{
    close();
}

std::string esp32_h264_isp_src::name() const
{
    return "esp32_h264_isp_src";
}

int esp32_h264_isp_src::push_encoder_input(component_pdu &&in)
{
    auto drain_to_out_q = [&] {
        component_pdu out;
        while (enc_->output(out) == 0)
        {
            std::lock_guard<std::mutex> lock(mu);
            if (out_q_.size() >= k_out_q_max)
            {
                out_q_.pop_front();
            }
            out_q_.push_back(std::move(out));
            out_cv_.notify_all();
            notify_wakeup();
        }
    };

    drain_to_out_q();
    const int r = enc_->input(std::move(in));
    if (0 == r)
    {
        drain_to_out_q();
    }
    return r;
}

void esp32_h264_isp_src::pipeline_main()
{
    while (!stop_.load(std::memory_order_acquire))
    {
        component_pdu in;
        const int       pull_r = cam_->wait_output(in, 100);
        if (pull_r != 0)
        {
            continue;
        }
        if (in.sdu_type != sdu_type_e::CAPS_VIDEO_RAW && in.sdu_type != sdu_type_e::O_UYY_E_VYY)
        {
            continue;
        }
        if (push_encoder_input(std::move(in)) < 0)
        {
            std::fprintf(stderr, "esp32_h264_isp_src: encode failed\n");
            break;
        }
    }
}

int esp32_h264_isp_src::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", buffers);
    (void)cam_->configure("buffers", buf);
    std::snprintf(buf, sizeof(buf), "%d", fps);
    (void)cam_->configure("fps", buf);

    const int cr = cam_->open();
    if (cr < 0)
    {
        return cr;
    }

    std::string size;
    cam_->query("size", &size);
    enc_->configure("size", size);
    std::snprintf(buf, sizeof(buf), "%" PRIu32, bitrate);
    enc_->configure("bitrate", buf);
    std::snprintf(buf, sizeof(buf), "%d", gop);
    enc_->configure("gop", buf);
    std::snprintf(buf, sizeof(buf), "%d", fps);
    enc_->configure("fps", buf);
    std::snprintf(buf, sizeof(buf), "%d", qp_min);
    enc_->configure("qp_min", buf);
    std::snprintf(buf, sizeof(buf), "%d", qp_max);
    enc_->configure("qp_max", buf);

    if (enc_->open() < 0)
    {
        cam_->close();
        return -EIO;
    }

    stop_.store(false, std::memory_order_release);
    thread_ = spawn_thread(thread_opts {"vs_h264_isp", k_pipeline_stack, 1, 12},
                           [this] { pipeline_main(); });
    opened = true;
    return 0;
}

void esp32_h264_isp_src::close()
{
    stop_.store(true, std::memory_order_release);
    out_cv_.notify_all();
    notify_wakeup();
    if (thread_.joinable())
    {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mu);
    if (enc_)
    {
        enc_->close();
    }
    if (cam_)
    {
        cam_->close();
    }
    out_q_.clear();
    opened = false;
    stop_.store(false, std::memory_order_release);
}

int esp32_h264_isp_src::output(component_pdu &out)
{
    std::unique_lock<std::mutex> lock(mu);
    if (!opened)
    {
        return -EBADF;
    }
    if (out_q_.empty())
    {
        out_cv_.wait_for(lock, std::chrono::milliseconds(50),
                         [&] { return !out_q_.empty() || !opened; });
    }
    if (out_q_.empty())
    {
        return -EAGAIN;
    }
    out = std::move(out_q_.front());
    out_q_.pop_front();
    return 0;
}

int esp32_h264_isp_src::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return -EBUSY;
    }
    if (key == "fps")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 1 || n > 120)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(n);
        return 0;
    }
    if (key == "gop")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 1 || n > 300)
        {
            return -EINVAL;
        }
        gop = static_cast<int>(n);
        return 0;
    }
    if (key == "bitrate" || key == "bps")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 25000)
        {
            return -EINVAL;
        }
        bitrate = static_cast<uint32_t>(n);
        return 0;
    }
    if (key == "qp_min")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 0 || n > 51)
        {
            return -EINVAL;
        }
        qp_min = static_cast<int>(n);
        return 0;
    }
    if (key == "qp_max")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 0 || n > 51)
        {
            return -EINVAL;
        }
        qp_max = static_cast<int>(n);
        return 0;
    }
    if (key == "buffers")
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || n < 2 || n > 8)
        {
            return -EINVAL;
        }
        buffers = static_cast<int>(n);
        return 0;
    }
    return cam_->configure(key, value);
}

int esp32_h264_isp_src::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    int r = port_caps_query(output_ports(), false, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }
    if (key == "status")
    {
        std::lock_guard<std::mutex> lock(mu);
        *value = opened ? "open" : "closed";
        return 0;
    }
    if (key == "backend")
    {
        *value = "esp_video_csi+esp_h264_hw";
        return 0;
    }
    if (key == "stats")
    {
        std::string cam_stats;
        std::string enc_stats;
        cam_->query("stats", &cam_stats);
        enc_->query("stats", &enc_stats);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "cam %s; enc %s", cam_stats.c_str(), enc_stats.c_str());
        *value = buf;
        return 0;
    }
    if (key == "encode_ms")
    {
        return enc_->query(key, value);
    }
    return cam_->query(key, value);
}

}  // namespace vstreamer
