#include "components/esp32p4_csi_camera_src.hpp"

#include "core/key_util.hpp"
#include "core/latency_query.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/thread_spawn.hpp"
#include "core/time_util.hpp"

#include <bfc/shared_sized_buffer.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"
#if CONFIG_CAMERA_OV5647
#include "ov5647_types.h"
#endif

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace vstreamer
{

struct esp32p4_csi_camera_src::ring
{
    std::mutex              mu;
    std::condition_variable cv;
    int                     fd = -1;
    bool                    streaming = false;
    std::vector<uint8_t *>  ptr;
    std::vector<uint32_t>   len;
    /* Dequeued and not yet back in the driver (in ready_q_ or downstream). */
    int held = 0;

    /* Release callback of a frame PDU; runs on whichever thread drops the last reference. */
    void requeue(uint32_t index)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (streaming)
        {
            struct v4l2_buffer b {};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = index;
            if (ioctl(fd, VIDIOC_QBUF, &b) != 0)
            {
                std::fprintf(stderr, "esp32p4_csi_camera_src: QBUF %" PRIu32 ": %s\n", index,
                             std::strerror(errno));
            }
        }
        --held;
        cv.notify_all();
    }
};

namespace
{

constexpr int k_min_bufs = 2;
constexpr int k_max_bufs = 8;
/* Bounds the capture thread's DQBUF so stop is seen while frames are held downstream. */
constexpr int k_dqbuf_timeout_ms = 100;
constexpr auto k_dqbuf_error_backoff = std::chrono::milliseconds(10);
/* How long close() waits for frames still downstream before it frees the driver buffers. */
constexpr auto k_release_wait = std::chrono::seconds(1);

std::mutex s_video_init_mu;
bool       s_video_init_done = false;

size_t frame_bytes(int w, int h)
{
    return static_cast<size_t>(w) * static_cast<size_t>(h) * 3 / 2;
}

int parse_int(std::string_view v, int64_t lo, int64_t hi, int *out)
{
    int64_t n = 0;
    if (key_parse_i64(v, &n) < 0 || n < lo || n > hi)
    {
        return -EINVAL;
    }
    *out = static_cast<int>(n);
    return 0;
}

int set_int(std::string *value, int64_t n)
{
    char buf[32];
    if (key_format_i64(n, buf, sizeof(buf)) < 0)
    {
        return -EINVAL;
    }
    *value = buf;
    return 0;
}

int parse_size(std::string_view s, int *w, int *h)
{
    const size_t x = s.find_first_of("xX");
    if (x == std::string_view::npos)
    {
        return -EINVAL;
    }
    int ww = 0;
    int hh = 0;
    /* O_UYY_E_VYY pairs lines and the chroma is 2x2: both even. 0x0 = the sensor's mode. */
    if (parse_int(s.substr(0, x), 0, 4096, &ww) < 0 || parse_int(s.substr(x + 1), 0, 4096, &hh) < 0 ||
        (ww % 2) || (hh % 2) || ((0 == ww) != (0 == hh)))
    {
        return -EINVAL;
    }
    *w = ww;
    *h = hh;
    return 0;
}

const std::vector<port_desc> &output_ports()
{
    static const std::vector<port_desc> ports = [] {
        port_desc       p;
        port_caps_entry caps {};
        caps.sdu_type = sdu_type_e::CAPS_VIDEO_RAW;
        p.caps.push_back(caps);
        port_caps_entry data {};
        data.sdu_type = sdu_type_e::O_UYY_E_VYY;
        p.caps.push_back(data);
        return std::vector<port_desc> {p};
    }();
    return ports;
}

int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
    if (ioctl(fd, req, arg) == 0)
    {
        return 0;
    }
    const int e = errno;
    std::fprintf(stderr, "esp32p4_csi_camera_src: %s: %s\n", what, std::strerror(e));
    return -(e ? e : EIO);
}

#if CONFIG_CAMERA_OV5647
/*
 * Also forces the anti-flicker band to the mains frequency: the 1080p table leaves 0x3c01 at
 * auto-detect, which can pick the wrong band and let lights flicker. The driver derives the
 * band step and count from the clock, HTS and VTS after loading the table.
 *
 * esp_cam_sensor's OV5647 1080p30 table says "24Minput" but its PLL (0x3035/0x3036) is the
 * Linux driver's, whose 81.6667 MHz pixel clock needs a 25 MHz xclk. At 24 MHz the clock is
 * 24/25 of that and HTS 2271 x VTS 1199 runs at 28.8 fps. Re-derive VTS for the requested
 * rate at 24 MHz and load the table as a custom sensor format.
 */
constexpr double k_ov5647_pclk_hz = 81666700.0 * 24.0 / 25.0;
constexpr int    k_ov5647_min_vts = 1104; /* 1080 active lines + the Linux driver's blanking */
constexpr int    k_ov5647_gain_ceiling = 0x1f0; /* real gain x16 */
std::vector<ov5647_reginfo_t> s_ov5647_regs;
esp_cam_sensor_format_t       s_ov5647_fmt;

void ov5647_fix_format(int fd, int fps, int light_freq)
{
    esp_cam_sensor_format_t cur {};
    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &cur) != 0 || nullptr == cur.name || nullptr == cur.regs ||
        std::strcmp(cur.name, "MIPI_2lane_24Minput_RAW10_1920x1080_30fps") != 0 || fps <= 0)
    {
        return;
    }
    /* cur.regs may already be s_ov5647_regs (a second open), so copy before replacing it. */
    const auto *src = static_cast<const ov5647_reginfo_t *>(cur.regs);
    std::vector<ov5647_reginfo_t> regs(src, src + cur.regs_size);
    int hts = 0;
    for (const auto &r : regs)
    {
        hts = 0x380c == r.reg ? (hts & 0xff) | (r.val << 8) : hts;
        hts = 0x380d == r.reg ? (hts & 0xff00) | r.val : hts;
    }
    if (hts <= 0)
    {
        return;
    }
    int vts = static_cast<int>(k_ov5647_pclk_hz / (static_cast<double>(hts) * fps));
    vts = vts < k_ov5647_min_vts ? k_ov5647_min_vts : vts;
    for (auto &r : regs)
    {
        r.val = 0x380e == r.reg ? static_cast<uint8_t>(vts >> 8) : r.val;
        r.val = 0x380f == r.reg ? static_cast<uint8_t>(vts & 0xff) : r.val;
    }
    if (0 != light_freq)
    {
        /* 0x3c01 bit 7: manual band; 0x3c00 bit 2: 50 Hz. Before the end marker. */
        uint8_t band = 0x40;
        regs.erase(std::remove_if(regs.begin(), regs.end(),
                                  [&](const ov5647_reginfo_t &r) {
                                      band = 0x3c00 == r.reg ? r.val : band;
                                      return 0x3c00 == r.reg || 0x3c01 == r.reg;
                                  }),
                   regs.end());
        band = 50 == light_freq ? (band | 0x04) : (band & ~0x04);
        const auto end = regs.empty() ? regs.end() : regs.end() - 1;
        regs.insert(end, {{0x3c00, band}, {0x3c01, 0x80}});
    }
    /* AGC ceiling 0x3a18/0x3a19: the table's 0x3ff (64x) lets the sensor's own AGC flip gain
     * between ~0x3ff and ~0x260 on alternate frames (a 15 Hz brightness flicker) once exposure
     * hits its band limit. At 0x1f0 (31x) the gain settles. */
    for (auto &r : regs)
    {
        r.val = 0x3a18 == r.reg ? static_cast<uint8_t>(k_ov5647_gain_ceiling >> 8) : r.val;
        r.val = 0x3a19 == r.reg ? static_cast<uint8_t>(k_ov5647_gain_ceiling & 0xff) : r.val;
    }
    s_ov5647_regs = std::move(regs);
    s_ov5647_fmt = cur;
    s_ov5647_fmt.regs = s_ov5647_regs.data();
    s_ov5647_fmt.regs_size = s_ov5647_regs.size();
    if (xioctl(fd, VIDIOC_S_SENSOR_FMT, &s_ov5647_fmt, "S_SENSOR_FMT ov5647 vts") == 0)
    {
        std::fprintf(stderr,
                     "esp32p4_csi_camera_src: ov5647 HTS %d VTS %d -> %.2f fps at 24 MHz xclk, "
                     "flicker band %s\n",
                     hts, vts, k_ov5647_pclk_hz / (static_cast<double>(hts) * vts),
                     50 == light_freq ? "50 Hz" : 60 == light_freq ? "60 Hz" : "auto");
    }
}
#endif

}  // namespace

esp32p4_csi_camera_src::esp32p4_csi_camera_src() = default;

esp32p4_csi_camera_src::~esp32p4_csi_camera_src()
{
    close();
}

std::string esp32p4_csi_camera_src::name() const
{
    return "esp32p4_csi_camera_src";
}

int esp32p4_csi_camera_src::capture_open_locked()
{
    {
        std::lock_guard<std::mutex> lock(s_video_init_mu);
        if (!s_video_init_done)
        {
            esp_video_init_csi_config_t csi {};
            csi.sccb_config.init_sccb = true;
            csi.sccb_config.i2c_config.port = static_cast<uint8_t>(sccb_port);
            csi.sccb_config.i2c_config.scl_pin = static_cast<gpio_num_t>(sccb_scl);
            csi.sccb_config.i2c_config.sda_pin = static_cast<gpio_num_t>(sccb_sda);
            csi.sccb_config.freq = static_cast<uint32_t>(sccb_freq);
            csi.reset_pin = static_cast<gpio_num_t>(reset_pin);
            csi.pwdn_pin = static_cast<gpio_num_t>(pwdn_pin);
            esp_video_init_config_t cfg {};
            cfg.csi = &csi;
            if (esp_video_init(&cfg) != ESP_OK)
            {
                std::fprintf(stderr,
                             "esp32p4_csi_camera_src: esp_video_init failed (no sensor on SCCB "
                             "sda=%d scl=%d?)\n",
                             sccb_sda, sccb_scl);
                return -ENODEV;
            }
            s_video_init_done = true;
        }
    }

    const int fd = ::open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0)
    {
        std::fprintf(stderr, "esp32p4_csi_camera_src: open %s: %s\n",
                     ESP_VIDEO_MIPI_CSI_DEVICE_NAME, std::strerror(errno));
        return -ENODEV;
    }
    auto r = std::make_shared<ring>();
    r->fd = fd;
    auto fail = [&](int err) {
        for (size_t i = 0; i < r->ptr.size(); ++i)
        {
            if (nullptr != r->ptr[i])
            {
                munmap(r->ptr[i], r->len[i]);
            }
        }
        ::close(fd);
        return err;
    };

    struct v4l2_capability cap {};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
    {
        driver = reinterpret_cast<const char *>(cap.driver);
    }
#if CONFIG_CAMERA_OV5647
    ov5647_fix_format(fd, fps, light_freq);
#endif

    struct v4l2_format fmt {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int err = xioctl(fd, VIDIOC_G_FMT, &fmt, "G_FMT");
    if (err < 0)
    {
        return fail(err);
    }
    if (width > 0)
    {
        fmt.fmt.pix.width = static_cast<uint32_t>(width);
        fmt.fmt.pix.height = static_cast<uint32_t>(height);
    }
    /* esp_video's YUV420 is the ISP's packed O_UYY_E_VYY, which the P4 H.264 core reads. */
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    err = xioctl(fd, VIDIOC_S_FMT, &fmt, "S_FMT YUV420");
    if (err < 0)
    {
        return fail(err);
    }
    err = xioctl(fd, VIDIOC_G_FMT, &fmt, "G_FMT");
    if (err < 0)
    {
        return fail(err);
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420 ||
        (width > 0 && (fmt.fmt.pix.width != static_cast<uint32_t>(width) ||
                       fmt.fmt.pix.height != static_cast<uint32_t>(height))))
    {
        std::fprintf(stderr, "esp32p4_csi_camera_src: sensor mode is %" PRIu32 "x%" PRIu32
                     ", not YUV420 %dx%d\n",
                     fmt.fmt.pix.width, fmt.fmt.pix.height, width, height);
        return fail(-EINVAL);
    }
    live_w = static_cast<int>(fmt.fmt.pix.width);
    live_h = static_cast<int>(fmt.fmt.pix.height);

    struct v4l2_streamparm parm {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0)
    {
        const uint32_t num = parm.parm.capture.timeperframe.numerator;
        const uint32_t den = parm.parm.capture.timeperframe.denominator;
        const float    driver_fps = (num > 0 && den > 0) ? static_cast<float>(den) / num : 0.f;
        std::fprintf(stderr,
                     "esp32p4_csi_camera_src: driver timeperframe %" PRIu32 "/%" PRIu32
                     " (~%.2f fps) cap=0x%" PRIx32 "\n",
                     num, den, static_cast<double>(driver_fps),
                     static_cast<uint32_t>(parm.parm.capture.capability));
        if (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)
        {
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator =
                fps > 0 ? static_cast<uint32_t>(fps) : 30U;
            (void)xioctl(fd, VIDIOC_S_PARM, &parm, "S_PARM fps");
        }
    }
    else
    {
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator =
            fps > 0 ? static_cast<uint32_t>(fps) : 30U;
        (void)xioctl(fd, VIDIOC_S_PARM, &parm, "S_PARM fps");
    }

    struct v4l2_requestbuffers req {};
    req.count = static_cast<uint32_t>(nbufs);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    err = xioctl(fd, VIDIOC_REQBUFS, &req, "REQBUFS");
    if (err < 0)
    {
        return fail(err);
    }
    r->ptr.assign(req.count, nullptr);
    r->len.assign(req.count, 0);
    for (uint32_t i = 0; i < req.count; ++i)
    {
        struct v4l2_buffer b {};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        err = xioctl(fd, VIDIOC_QUERYBUF, &b, "QUERYBUF");
        if (err < 0)
        {
            return fail(err);
        }
        void *p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (MAP_FAILED == p)
        {
            return fail(-ENOMEM);
        }
        r->ptr[i] = static_cast<uint8_t *>(p);
        r->len[i] = b.length;
        if (b.length < frame_bytes(live_w, live_h))
        {
            return fail(-EINVAL);
        }
    }

    struct timeval tv {};
    tv.tv_usec = k_dqbuf_timeout_ms * 1000;
    err = xioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &tv, "S_DQBUF_TIMEOUT");
    if (err < 0)
    {
        return fail(err);
    }
    for (uint32_t i = 0; i < req.count; ++i)
    {
        struct v4l2_buffer b {};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        err = xioctl(fd, VIDIOC_QBUF, &b, "QBUF");
        if (err < 0)
        {
            return fail(err);
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    err = xioctl(fd, VIDIOC_STREAMON, &type, "STREAMON");
    if (err < 0)
    {
        return fail(err);
    }
    r->streaming = true;
    ring_ = std::move(r);
    return 0;
}

void esp32p4_csi_camera_src::release_ring(std::shared_ptr<ring> r)
{
    std::unique_lock<std::mutex> lock(r->mu);
    r->streaming = false;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)ioctl(r->fd, VIDIOC_STREAMOFF, &type);
    if (!r->cv.wait_for(lock, k_release_wait, [&] { return r->held <= 0; }))
    {
        /* Closing the device frees the buffers; a consumer still reads one. Leak instead. */
        std::fprintf(stderr,
                     "esp32p4_csi_camera_src: %d frame(s) still held downstream; leaving %s "
                     "open\n",
                     r->held, ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return;
    }
    for (size_t i = 0; i < r->ptr.size(); ++i)
    {
        munmap(r->ptr[i], r->len[i]);
    }
    ::close(r->fd);
    r->fd = -1;
}

void esp32p4_csi_camera_src::capture_main()
{
    std::shared_ptr<ring> r;
    size_t                want = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        r = ring_;
        want = frame_bytes(live_w, live_h);
    }
    if (!r)
    {
        return;
    }
    while (!stop_.load(std::memory_order_acquire))
    {
        struct v4l2_buffer b {};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        /* Blocks up to k_dqbuf_timeout_ms; times out while every buffer is downstream. */
        if (ioctl(r->fd, VIDIOC_DQBUF, &b) != 0)
        {
            if (ETIMEDOUT != errno)
            {
                {
                    std::lock_guard<std::mutex> lock(mu);
                    ++dqbuf_errors_;
                }
                /* A driver in an error state fails at once: do not spin core 0. */
                std::this_thread::sleep_for(k_dqbuf_error_backoff);
            }
            continue;
        }
        /* esp_video leaves the V4L2 timestamp at 0. */
        const uint64_t ts_us = static_cast<uint64_t>(steady_mono_ns() / 1000);
        if (b.index >= r->ptr.size())
        {
            std::lock_guard<std::mutex> lock(mu);
            ++dqbuf_errors_;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(r->mu);
            ++r->held;
        }
        /* Short or errored frame (bytesused 0): straight back to the driver. */
        if (b.bytesused != want)
        {
            {
                std::lock_guard<std::mutex> lock(mu);
                ++dqbuf_errors_;
            }
            r->requeue(b.index);
            continue;
        }

        /* No cache sync: the CPU never reads the pixels and the H.264 DMA reads PSRAM. */
        const uint32_t index = b.index;

        component_pdu pdu;
        pdu.ts_us = ts_us;
        pdu.sdu_type = sdu_type_e::O_UYY_E_VYY;
        pdu.port = 0;
        pdu.sdu = bfc::shared_sized_buffer::adopt(
            reinterpret_cast<std::byte *>(r->ptr[index]), r->len[index], b.bytesused,
            [r, index](std::byte *) { r->requeue(index); });

        std::optional<component_pdu> stale;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (ready_q_.size() >= k_ready_q_max)
            {
                stale = std::move(ready_q_.front());
                ready_q_.pop_front();
                ++dropped_;
            }
            ready_q_.push_back(std::move(pdu));
            ++frames_;
            if (frames_ > 1)
            {
                sum_dq_interval_us_ += ts_us - last_dq_ts_us_;
                ++dq_interval_samples_;
            }
            last_dq_ts_us_ = ts_us;
        }
        /* stale's buffer is requeued here, outside mu. */
        stale.reset();
        notify_wakeup();
        out_cv_.notify_one();
#if defined(ESP_PLATFORM)
        {
            /* Under mu: set_wake_task(nullptr) runs on the woken task before it exits, so the
             * handle is alive for as long as it is set. */
            std::lock_guard<std::mutex> lock(mu);
            if (nullptr != wake_task_)
            {
                xTaskNotifyGive(static_cast<TaskHandle_t>(wake_task_));
            }
        }
#endif
    }
}

#if defined(ESP_PLATFORM)
void esp32p4_csi_camera_src::set_wake_task(void *task_handle)
{
    std::lock_guard<std::mutex> lock(mu);
    wake_task_ = task_handle;
}
#endif

int esp32p4_csi_camera_src::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (source_open)
    {
        return 0;
    }
    const int r = capture_open_locked();
    if (r < 0)
    {
        return r;
    }
    source_open = true;
    caps_sent_ = false;
    stop_.store(false, std::memory_order_release);
    /* capture_main() waits for mu, so it starts once open() returns. */
    thread_ = spawn_thread(thread_opts {"vs_csi_cam", 8 * 1024, 0, 12},
                           [this] { capture_main(); });
    return 0;
}

void esp32p4_csi_camera_src::close()
{
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!source_open)
        {
            return;
        }
        source_open = false;
#if defined(ESP_PLATFORM)
        wake_task_ = nullptr;
#endif
        out_cv_.notify_all();
    }
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable())
    {
        thread_.join();
    }
    std::shared_ptr<ring>        r;
    std::optional<component_pdu> last;
    {
        std::lock_guard<std::mutex> lock(mu);
        r = std::move(ring_);
        while (!ready_q_.empty())
        {
            last = std::move(ready_q_.front());
            ready_q_.pop_front();
        }
    }
    /* Outside mu: a consumer may block in output() while it still holds a frame. */
    last.reset();
    if (r)
    {
        release_ring(std::move(r));
    }
}

int esp32p4_csi_camera_src::try_pop_output_locked(component_pdu &out)
{
    if (!source_open)
    {
        return -EBADF;
    }
    if (!caps_sent_)
    {
        video_raw_caps caps {};
        caps.width = live_w;
        caps.height = live_h;
        caps.hor_stride = live_w;
        caps.ver_stride = live_h;
        caps.fps_num = fps;
        caps.fps_den = 1;
        out = make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps,
                            static_cast<uint64_t>(steady_mono_ns() / 1000), 0);
        out.seq = out_seq_++;
        caps_sent_ = true;
        return 0;
    }
    if (ready_q_.empty())
    {
        return -EAGAIN;
    }
    out = std::move(ready_q_.front());
    ready_q_.pop_front();
    out.seq = out_seq_++;
    latency_store(latency_ms_, steady_mono_ns(), static_cast<int64_t>(out.ts_us) * 1000);
    return 0;
}

int esp32p4_csi_camera_src::output(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu);
    return try_pop_output_locked(out);
}

int esp32p4_csi_camera_src::wait_output(component_pdu &out, int timeout_ms)
{
    if (timeout_ms < 0)
    {
        timeout_ms = 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mu);
    for (;;)
    {
        const int r = try_pop_output_locked(out);
        if (0 == r || -EBADF == r)
        {
            return r;
        }
        if (-EAGAIN != r)
        {
            return r;
        }
        if (!source_open || stop_.load(std::memory_order_acquire))
        {
            return -EBADF;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return -EAGAIN;
        }
#if defined(ESP_PLATFORM)
        if (nullptr != wake_task_)
        {
            /* Whatever is left of timeout_ms, so a stalled sensor still returns -EAGAIN. */
            const auto left_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1;
            lock.unlock();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left_ms));
            lock.lock();
            continue;
        }
#endif
        out_cv_.wait_until(lock, deadline, [this] {
            return !ready_q_.empty() || !source_open ||
                   stop_.load(std::memory_order_acquire);
        });
        if (ready_q_.empty() && source_open && !stop_.load(std::memory_order_acquire))
        {
            return -EAGAIN;
        }
    }
}

int esp32p4_csi_camera_src::configure(std::string_view key, std::string_view value)
{
    std::lock_guard<std::mutex> lock(mu);
    if (key == "fps")
    {
        return parse_int(value, 1, 120, &fps);
    }
    /* The rest take effect on the next open(). */
    if (source_open)
    {
        return -EBUSY;
    }
    if (key == "size")
    {
        return parse_size(value, &width, &height);
    }
    if (key == "buffers")
    {
        return parse_int(value, k_min_bufs, k_max_bufs, &nbufs);
    }
    if (key == "light_freq")
    {
        int f = 0;
        if (parse_int(value, 0, 60, &f) < 0 || (0 != f && 50 != f && 60 != f))
        {
            return -EINVAL;
        }
        light_freq = f;
        return 0;
    }
    if (key == "sccb_port")
    {
        return parse_int(value, 0, 1, &sccb_port);
    }
    if (key == "sccb_scl")
    {
        return parse_int(value, 0, 63, &sccb_scl);
    }
    if (key == "sccb_sda")
    {
        return parse_int(value, 0, 63, &sccb_sda);
    }
    if (key == "sccb_freq")
    {
        return parse_int(value, 10000, 1000000, &sccb_freq);
    }
    if (key == "reset_pin")
    {
        return parse_int(value, -1, 63, &reset_pin);
    }
    if (key == "pwdn_pin")
    {
        return parse_int(value, -1, 63, &pwdn_pin);
    }
    return -ENOTSUP;
}

int esp32p4_csi_camera_src::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    const int r = port_caps_query(output_ports(), false, key, value);
    if (0 == r || -EINVAL == r)
    {
        return r;
    }

    if (const int lr = latency_query(key, latency_ms_, latency_ms_, value); -ENOTSUP != lr)
    {
        return lr;
    }
    std::lock_guard<std::mutex> lock(mu);
    if (key == "state" || key == "status")
    {
        *value = source_open ? "capturing" : "closed";
        return 0;
    }
    if (key == "size")
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dx%d", source_open ? live_w : width,
                      source_open ? live_h : height);
        *value = buf;
        return 0;
    }
    if (key == "fps")
    {
        return set_int(value, fps);
    }
    if (key == "buffers")
    {
        return set_int(value, nbufs);
    }
    if (key == "light_freq")
    {
        return set_int(value, light_freq);
    }
    if (key == "driver")
    {
        *value = driver;
        return 0;
    }
    if (key == "stats")
    {
        char buf[128];
        const double dq_ms =
            dq_interval_samples_ > 0
                ? static_cast<double>(sum_dq_interval_us_) /
                      static_cast<double>(dq_interval_samples_) / 1000.0
                : 0.0;
        std::snprintf(buf, sizeof(buf),
                      "frames=%" PRIu64 " dropped=%" PRIu64 " dqbuf_errors=%" PRIu64
                      " dq_ms=%.2f",
                      frames_, dropped_, dqbuf_errors_, dq_ms);
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
