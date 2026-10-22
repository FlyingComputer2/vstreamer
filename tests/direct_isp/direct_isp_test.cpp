// Host contract tests: real source and factory, deterministic camera/codec
// doubles. The doubles model synchronous DMA reads and the driver's buffer
// return callback.
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "components/esp32p4_csi_device.hpp"
#include "components/h264_encoder_esp32p4.hpp"
#include "components/esp32_h264_isp_ng_src.hpp"
#include "core/component_factory.hpp"
#include "core/key_util.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/time_util.hpp"

using namespace std::chrono_literals;
namespace
{
void check(bool value, const char *message)
{
    if (!value)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}
struct fake_hardware
{
    std::mutex mu;
    std::condition_variable cv;
    std::array<std::byte, 24> camera_buffer{};
    bool device_open = false, hold_encode = false, encode_active = false;
    bool closed_during_encode = false;
    bool same_address = true;
    int pending = 0, dequeues = 0, returns = 0, encodes = 0;
    int encoder_control_calls = 0;
    int encode_result = 0, camera_open_result = 0;
    std::map<std::string, std::string> camera_settings, encoder_settings;
    void reset()
    {
        std::lock_guard<std::mutex> lock(mu);
        device_open = hold_encode = encode_active = closed_during_encode = false;
        same_address = true;
        pending = dequeues = returns = encodes = 0;
        encoder_control_calls = 0;
        encode_result = camera_open_result = 0;
        camera_settings.clear();
        encoder_settings.clear();
    }
    void feed()
    {
        std::lock_guard<std::mutex> lock(mu);
        ++pending;
        cv.notify_all();
    }
} hw;
void eventually(const std::function<bool()> &condition, const char *message)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!condition())
    {
        check(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
int count(int fake_hardware::*field)
{
    std::lock_guard<std::mutex> lock(hw.mu);
    return hw.*field;
}
vstreamer::component_pdu next(vstreamer::component_source &source)
{
    vstreamer::component_pdu pdu;
    eventually([&] { return source.output(pdu) == 0; }, "output timed out");
    return pdu;
}

void check_output_during_encoder_control(const char *key, bool configure)
{
    using namespace vstreamer;
    hw.reset();
    auto source = component_factory::create_source("esp32_h264_isp_ng_src");
    check(source && source->open() == 0, "control concurrency source opens");
    check(next(*source).sdu_type == sdu_type_e::CAPS_VIDEO_CODED, "control concurrency caps");
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        hw.hold_encode = true;
    }
    hw.feed();
    eventually(
        []
        {
            std::lock_guard<std::mutex> lock(hw.mu);
            return hw.encode_active;
        },
        "control concurrency encoder entered");
    const int calls_before = count(&fake_hardware::encoder_control_calls);
    auto control = std::async(std::launch::async, [&]
    {
        std::string value;
        return configure ? source->configure(key, "8000000") : source->query(key, &value);
    });
    eventually(
        [&] { return count(&fake_hardware::encoder_control_calls) > calls_before; },
        "control call reached the busy encoder");
    check(control.wait_for(0ms) == std::future_status::timeout,
          "encoder control waits for synchronous encode");
    auto reading = std::async(std::launch::async, [&]
    {
        component_pdu out;
        return source->output(out);
    });
    check(reading.wait_for(200ms) == std::future_status::ready,
          "output does not wait for encoder query/configuration");
    check(reading.get() == -EAGAIN, "empty output during encoder control returns EAGAIN");
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        check(hw.encode_active, "output returned while encoding was still in progress");
        hw.hold_encode = false;
        hw.cv.notify_all();
    }
    check(control.wait_for(2s) == std::future_status::ready, "encoder control finishes after DMA");
    check(control.get() == 0, "encoder control succeeds");
    source->close();
}
}  // namespace

// Only the device boundary is replaced: no ready queue or capture worker.
namespace vstreamer::detail
{
esp32p4_csi_device::~esp32p4_csi_device()
{
    close();
}
int esp32p4_csi_device::open()
{
    std::lock_guard<std::mutex> lock(hw.mu);
    if (hw.camera_open_result)
    {
        return hw.camera_open_result;
    }
    hw.device_open = true;
    live_w = live_h = 4;
    return 0;
}
void esp32p4_csi_device::close()
{
    std::lock_guard<std::mutex> lock(hw.mu);
    hw.closed_during_encode |= hw.encode_active;
    hw.device_open = false;
}
int esp32p4_csi_device::dequeue(component_pdu &out)
{
    std::unique_lock<std::mutex> lock(hw.mu);
    if (!hw.cv.wait_for(lock, 5ms, [] { return hw.pending != 0; }))
    {
        return -EAGAIN;
    }
    --hw.pending;
    ++hw.dequeues;
    check(hw.returns == hw.dequeues - 1, "driver buffer reused before its release");
    out.ts_us = steady_mono_ns() / 1000;
    out.sdu_type = sdu_type_e::O_UYY_E_VYY;
    out.sdu = bfc::shared_sized_buffer::adopt(
        hw.camera_buffer.data(), 24, 24,
        [](std::byte *)
        {
            std::lock_guard<std::mutex> lock(hw.mu);
            check(!hw.encode_active, "camera buffer returned during encoder DMA");
            ++hw.returns;
        });
    return 0;
}
int esp32p4_csi_device::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(hw.mu);
    hw.camera_settings[std::string(key)] = value;
    if (key == "fps")
    {
        fps = std::stoi(std::string(value));
    }
    return 0;
}
int esp32p4_csi_device::query(std::string_view key, std::string *value) const
{
    if (key == "size")
    {
        *value = "4x4";
        return 0;
    }
    if (key == "fps")
    {
        *value = std::to_string(fps);
        return 0;
    }
    return -ENOTSUP;
}
}  // namespace vstreamer::detail
namespace vstreamer
{
h264_encoder_esp32p4::h264_encoder_esp32p4() = default;
h264_encoder_esp32p4::~h264_encoder_esp32p4() = default;
std::string h264_encoder_esp32p4::name() const
{
    return "fake_encoder";
}
int h264_encoder_esp32p4::open()
{
    std::lock_guard<std::mutex> lock(mu);
    return 0;
}
void h264_encoder_esp32p4::close()
{
    std::lock_guard<std::mutex> lock(mu);
    out_q_.clear();
}
int h264_encoder_esp32p4::input(component_pdu &&in)
{
    // Production holds this mutex across the synchronous hardware encode.
    std::lock_guard<std::mutex> encoder_lock(mu);
    if (in.sdu_type == sdu_type_e::CAPS_VIDEO_RAW)
    {
        video_coded_caps caps{};
        caps.width = caps.height = 4;
        out_q_.push_back(make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, caps, in.ts_us, 0));
        return 0;
    }
    std::unique_lock<std::mutex> lock(hw.mu);
    hw.same_address &= in.sdu.data() == hw.camera_buffer.data();
    hw.encode_active = true;
    hw.cv.notify_all();
    hw.cv.wait(lock, [] { return !hw.hold_encode; });
    hw.encode_active = false;
    ++hw.encodes;
    if (hw.encode_result)
    {
        return hw.encode_result;
    }
    component_pdu coded;
    coded.ts_us = in.ts_us;
    coded.sdu_type = sdu_type_e::H264_AU;
    out_q_.push_back(std::move(coded));
    return 0;
}
int h264_encoder_esp32p4::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu);
    if (out_q_.empty())
    {
        return -EAGAIN;
    }
    out = std::move(out_q_.front());
    out_q_.pop_front();
    return 0;
}
int h264_encoder_esp32p4::configure(std::string_view key, std::string_view value)
{
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        ++hw.encoder_control_calls;
    }
    std::lock_guard<std::mutex> encoder_lock(mu);
    std::lock_guard<std::mutex> lock(hw.mu);
    hw.encoder_settings[std::string(key)] = value;
    return 0;
}
int h264_encoder_esp32p4::query(std::string_view key, std::string *value) const
{
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        ++hw.encoder_control_calls;
    }
    std::lock_guard<std::mutex> lock(mu);
    if (key == "stats")
    {
        *value = "fake";
        return 0;
    }
    if (key == "enc_frames")
    {
        *value = std::to_string(count(&fake_hardware::encodes));
        return 0;
    }
    return -ENOTSUP;
}
}  // namespace vstreamer

int main()
{
    using namespace vstreamer;
    hw.reset();
    auto source = component_factory::create_source("esp32_h264_isp_ng_src");
    check(source && source->name() == "esp32_h264_isp_ng_src", "factory selects direct source");
    check(source->configure("sccb_sda", "7") == 0, "camera config");
    check(source->configure("rc", "frame") == 0, "encoder config");
    check(source->configure("fps", "25") == 0, "shared fps config");
    check(source->open() == 0, "open");
    check(next(*source).sdu_type == sdu_type_e::CAPS_VIDEO_CODED, "coded caps first");
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        check(hw.camera_settings["sccb_sda"] == "7", "camera setting routed");
        check(hw.encoder_settings["rc"] == "frame", "encoder setting routed");
        check(hw.encoder_settings["fps"] == "25", "camera fps reaches encoder");
    }
    check(source->configure("fps", "30") == -EBUSY, "live camera change rejected");
    check(source->configure("qp_min", "10") == -EBUSY, "live reopen rejected");
    check(source->configure("bitrate", "8000000") == 0, "live bitrate accepted");
    check(source->configure("idr", "1") == 0, "live IDR accepted");
    check(source->configure("unknown", "1") == -ENOTSUP, "unknown key rejected");
    hw.feed();
    eventually([] { return count(&fake_hardware::returns) == 1; }, "buffer returned after encode");
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        check(hw.same_address, "same DMA address at encode");
    }
    // Keep the coded AU waiting: next raw frame must be returned without
    // encoding.
    hw.feed();
    eventually([] { return count(&fake_hardware::returns) == 2; }, "backpressure frame returned");
    check(count(&fake_hardware::encodes) == 1, "backpressure skips raw encode");
    check(next(*source).sdu_type == sdu_type_e::H264_AU, "coded AU preserved");
    component_pdu empty;
    check(source->output(empty) == -EAGAIN, "empty output is nonblocking");
    // Closing during encode must wait before tearing down the camera mapping.
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        hw.hold_encode = true;
    }
    hw.feed();
    eventually(
        []
        {
            std::lock_guard<std::mutex> lock(hw.mu);
            return hw.encode_active;
        },
        "encoder entered");
    auto closing = std::async(std::launch::async, [&] { source->close(); });
    check(closing.wait_for(20ms) == std::future_status::timeout,
          "close waits for synchronous encode");
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        hw.hold_encode = false;
        hw.cv.notify_all();
    }
    check(closing.wait_for(2s) == std::future_status::ready, "close finishes after DMA");
    closing.get();
    check(count(&fake_hardware::returns) == 3, "in-flight buffer returned at shutdown");
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        check(!hw.closed_during_encode, "mapping alive during DMA");
    }
    check(source->output(empty) == -EBADF, "output after close");
    source->close();
    check(source->open() == 0, "reopen");
    check(next(*source).sdu_type == sdu_type_e::CAPS_VIDEO_CODED, "fresh caps after reopen");
    // A recoverable overflow returns the raw buffer and leaves the worker
    // running.
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        hw.encode_result = -EIO;
    }
    hw.feed();
    eventually([] { return count(&fake_hardware::returns) == 4; }, "overflow returns buffer");
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        hw.encode_result = 0;
    }
    hw.feed();
    check(next(*source).sdu_type == sdu_type_e::H264_AU, "worker recovers after overflow");
    // Exhausted coded-buffer slots must return raw ownership without a spin/retry.
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        hw.encode_result = -EAGAIN;
    }
    const int returned_before = count(&fake_hardware::returns);
    hw.feed();
    eventually([&] { return count(&fake_hardware::returns) == returned_before + 1; },
               "encoder backpressure returns raw buffer");
    check(source->output(empty) == -EAGAIN, "encoder backpressure is nonfatal");
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        hw.encode_result = -EINVAL;
    }
    hw.feed();
    eventually([&] { return source->output(empty) == -EINVAL; }, "fatal encode error propagated");
    source->close();
    hw.reset();
    {
        std::lock_guard<std::mutex> lock(hw.mu);
        hw.camera_open_result = -ENODEV;
    }
    check(source->open() == -ENODEV, "camera open failure propagated");
    source->close();
    source.reset();
    check_output_during_encoder_control("stats", false);
    check_output_during_encoder_control("enc_frames", false);
    check_output_during_encoder_control("bitrate", true);
    check_output_during_encoder_control("reset_stats", true);
    // The existing camera node must preserve shared frame ownership after the
    // driver plumbing is extracted into the common helper.
    hw.reset();
    auto camera = component_factory::create_source("csi_camera");
    check(camera && camera->open() == 0, "existing camera node opens");
    check(next(*camera).sdu_type == sdu_type_e::CAPS_VIDEO_RAW, "camera raw caps first");
    hw.feed();
    component_pdu raw = next(*camera);
    check(raw.sdu_type == sdu_type_e::O_UYY_E_VYY, "camera raw frame");
    check(count(&fake_hardware::returns) == 0, "camera output holds driver buffer");
    component_pdu shared = raw;
    raw.sdu = {};
    check(count(&fake_hardware::returns) == 0, "shared camera frame retains buffer");
    shared.sdu = {};
    check(count(&fake_hardware::returns) == 1, "last camera reference returns buffer");
    camera->close();
    check(camera->output(empty) == -EBADF, "camera closed output");
    check(camera->open() == 0, "camera reopen");
    check(next(*camera).sdu_type == sdu_type_e::CAPS_VIDEO_RAW, "camera fresh caps after reopen");
    camera->close();
    std::cout << "PASS: factory/configuration, zero-copy buffer identity, "
                 "backpressure, DMA lifetime, shutdown, reopen, recovery and "
                 "failure propagation, nonblocking output during encoder control\n";
}
