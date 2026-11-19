#include "components/esp32p4_csi_device.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <bfc/shared_sized_buffer.hpp>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include "components/camera_image_ctl.hpp"
#include "core/key_util.hpp"
#include "core/sdu_type.hpp"
#include "core/time_util.hpp"
#include "driver/i2c_master.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_video_isp_ioctl.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"
#if CONFIG_CAMERA_OV5647
#include "ov5647_types.h"
#endif

namespace vstreamer::detail
{

struct esp32p4_csi_device::ring
{
    std::mutex mu;
    std::condition_variable cv;
    int fd = -1;
    bool streaming = false;
    std::vector<uint8_t *> ptr;
    std::vector<uint32_t> len;
    /* Dequeued and not yet returned by the last frame PDU reference. */
    int held = 0;

    /* Release callback of a frame PDU; runs on whichever thread drops the last
     * reference. */
    void requeue(uint32_t index)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (streaming)
        {
            struct v4l2_buffer b
            {
            };
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

/*
 * Live image controls. Sensor keys write OV5647 registers through a device
 * handle of our own on the camera's SCCB bus (esp_video has no register
 * ioctl; the i2c master serializes our transfers with esp_cam_sensor's). ISP
 * keys go to the ISP video device, which esp_video lets several users open.
 *
 * The ISP tuning (esp_ipa) writes contrast, gamma and sharpen once its first
 * statistics arrive, which can be after STREAMON, so the ISP keys that were set
 * are written again a few frames into each stream.
 */
struct esp32p4_csi_device::image_ctl
{
    std::mutex mu;
    int value[camctl::k_key_count];
    bool set[camctl::k_key_count] = {};
    i2c_master_dev_handle_t sccb = nullptr;
    bool sensor_ok = false; /* chip id read back as 0x5647 */
    int hts = 0, vts = 0;   /* read from the sensor at each stream start */
    int isp_fd = -1;
    bool sharpen_base_valid = false;
    esp_video_isp_sharpen_t sharpen_base{};
    /* Frame count of this stream and the frame at which to write the ISP keys
     * again; 0 = done. Touched by dequeue() only. */
    uint32_t frames = 0;
    uint32_t reapply_at = 0;

    image_ctl();
    ~image_ctl();
    int v(std::string_view name) const
    {
        return value[camctl::find_key(name)];
    }
    void attach_sensor(int port, int freq);
    int sensor_read(uint16_t reg, uint8_t *out);
    int sensor_write(uint16_t reg, uint8_t val);
    int sensor_bits(uint16_t reg, uint8_t mask, bool on);
    void read_timing();
    int isp_open();
    int isp_set(uint32_t id, int32_t val);
    int isp_set_struct(uint32_t id, void *p, size_t size);
    int isp_get_struct(uint32_t id, void *p, size_t size);
    int apply(int index);
    void apply_set(bool isp_only);
    int sensor_state(std::string *out);
    int isp_state(std::string *out);
};

namespace
{

constexpr int k_min_bufs = 2;
constexpr int k_max_bufs = 8;
/* Bounds DQBUF so the owning worker can observe a stop request. */
constexpr int k_dqbuf_timeout_ms = 100;
/* How long close() waits for frames still downstream before it frees the driver
 * buffers. */
constexpr auto k_release_wait = std::chrono::seconds(1);

std::mutex s_video_init_mu;
bool s_video_init_done = false;

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
    /* O_UYY_E_VYY pairs lines and the chroma is 2x2: both even. 0x0 = the
     * sensor's mode. */
    if (parse_int(s.substr(0, x), 0, 4096, &ww) < 0 ||
        parse_int(s.substr(x + 1), 0, 4096, &hh) < 0 || (ww % 2) || (hh % 2) ||
        ((0 == ww) != (0 == hh)))
    {
        return -EINVAL;
    }
    *w = ww;
    *h = hh;
    return 0;
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
 * Also forces the anti-flicker band to the mains frequency: the 1080p table
 * leaves 0x3c01 at auto-detect, which can pick the wrong band and let lights
 * flicker. The driver derives the band step and count from the clock, HTS and
 * VTS after loading the table.
 *
 * esp_cam_sensor's OV5647 1080p30 table says "24Minput" but its PLL
 * (0x3035/0x3036) is the Linux driver's, whose 81.6667 MHz pixel clock needs a
 * 25 MHz xclk. At 24 MHz the clock is 24/25 of that and HTS 2271 x VTS 1199
 * runs at 28.8 fps. Re-derive VTS for the requested rate at 24 MHz and load the
 * table as a custom sensor format.
 */
constexpr double k_ov5647_pclk_hz = 81666700.0 * 24.0 / 25.0;
constexpr int k_ov5647_min_vts = 1104; /* 1080 active lines + the Linux driver's blanking */
std::vector<ov5647_reginfo_t> s_ov5647_regs;
esp_cam_sensor_format_t s_ov5647_fmt;
/* ISP description with the Bayer order of the flipped readout. Placement-new
 * storage: the struct has a const member, so it cannot be assigned. */
alignas(esp_cam_sensor_isp_info_t) unsigned char s_ov5647_isp_storage[sizeof(esp_cam_sensor_isp_info_t)];

/* Set bit 1 (vertical flip in 0x3820, horizontal mirror in 0x3821) of every
 * entry of a register in the table, adding the register before the end marker
 * if absent. Absolute, not a toggle: cur.regs may already be a patched table
 * from an earlier open. */
void ov5647_set_bit1(std::vector<ov5647_reginfo_t> &regs, uint16_t reg, bool on)
{
    bool found = false;
    for (auto &r : regs)
    {
        if (reg == r.reg)
        {
            r.val = static_cast<uint8_t>((r.val & ~0x02) | (on ? 0x02 : 0));
            found = true;
        }
    }
    if (!found)
    {
        const auto end = regs.empty() ? regs.end() : regs.end() - 1;
        regs.insert(end, ov5647_reginfo_t{reg, static_cast<uint8_t>(on ? 0x02 : 0)});
    }
}

void ov5647_fix_format(int fd, int fps, int light_freq, int rotate, int gain_ceiling)
{
    esp_cam_sensor_format_t cur{};
    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &cur) != 0 || nullptr == cur.name || nullptr == cur.regs ||
        std::strcmp(cur.name, "MIPI_2lane_24Minput_RAW10_1920x1080_30fps") != 0 || fps <= 0)
    {
        std::fprintf(stderr, "esp32p4_csi_camera_src: ov5647 mode not patched (rotate %d ignored)\n",
                     rotate);
        return;
    }
    /* cur.regs may already be s_ov5647_regs (a second open), so copy before
     * replacing it. */
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
        /* 0x3c01 bit 7: manual band; 0x3c00 bit 2: 50 Hz. Before the end
         * marker. */
        uint8_t band = 0x40;
        regs.erase(std::remove_if(regs.begin(), regs.end(),
                                  [&](const ov5647_reginfo_t &r)
                                  {
                                      band = 0x3c00 == r.reg ? r.val : band;
                                      return 0x3c00 == r.reg || 0x3c01 == r.reg;
                                  }),
                   regs.end());
        band = 50 == light_freq ? (band | 0x04) : (band & ~0x04);
        const auto end = regs.empty() ? regs.end() : regs.end() - 1;
        regs.insert(end, {{0x3c00, band}, {0x3c01, 0x80}});
    }
    /* AGC ceiling 0x3a18/0x3a19: the table's 0x3ff (64x) lets the sensor's own
     * AGC flip gain between ~0x3ff and ~0x260 on alternate frames (a 15 Hz
     * brightness flicker) once exposure hits its band limit. At 0x1f0 (31x) the
     * gain settles. The gain_ceiling key overrides it. */
    for (auto &r : regs)
    {
        r.val = 0x3a18 == r.reg ? static_cast<uint8_t>(gain_ceiling >> 8) : r.val;
        r.val = 0x3a19 == r.reg ? static_cast<uint8_t>(gain_ceiling & 0xff) : r.val;
    }
    /* The table's native orientation is flip off, mirror on (0x3820 bit 1 = 0,
     * 0x3821 bit 1 = 1); 180 degrees inverts both. */
    ov5647_set_bit1(regs, 0x3820, 180 == rotate);
    ov5647_set_bit1(regs, 0x3821, 180 != rotate);
    /* Rotating the mosaic 180 degrees turns the sensor's GBRG into GRBG; the ISP
     * has to demosaic the order the sensor now outputs. Absolute, like the
     * registers: the table's native order is GBRG. */
    if (nullptr != cur.isp_info)
    {
        const esp_cam_sensor_isp_info_t native(*cur.isp_info);
        auto *isp = new (s_ov5647_isp_storage) esp_cam_sensor_isp_info_t(native);
        isp->isp_v1_info.bayer_type = 180 == rotate ? ESP_CAM_SENSOR_BAYER_GRBG : ESP_CAM_SENSOR_BAYER_GBRG;
        cur.isp_info = isp;
    }
    s_ov5647_regs = std::move(regs);
    s_ov5647_fmt = cur;
    s_ov5647_fmt.regs = s_ov5647_regs.data();
    s_ov5647_fmt.regs_size = s_ov5647_regs.size();
    if (xioctl(fd, VIDIOC_S_SENSOR_FMT, &s_ov5647_fmt, "S_SENSOR_FMT ov5647 vts") == 0)
    {
        std::fprintf(stderr,
                     "esp32p4_csi_camera_src: ov5647 HTS %d VTS %d -> %.2f fps "
                     "at 24 MHz xclk, "
                     "flicker band %s\n",
                     hts, vts, k_ov5647_pclk_hz / (static_cast<double>(hts) * vts),
                     50 == light_freq   ? "50 Hz"
                     : 60 == light_freq ? "60 Hz"
                                        : "auto");
    }
}
#endif

/* ISP frames to wait after STREAMON before writing the set ISP keys again,
 * past the tuning's first update. */
constexpr uint32_t k_isp_reapply_frame = 8;
constexpr int k_sccb_timeout_ms = 50;
constexpr uint16_t k_ov5647_addr = 0x36;
/* The sharpen coefficients are 3.5 fixed point. */
constexpr float k_sharpen_coeff_max = 7.96875f;

}  // namespace

/* ----- image controls ----- */

esp32p4_csi_device::image_ctl::image_ctl()
{
    for (int i = 0; i < camctl::k_key_count; ++i)
    {
        value[i] = 0;
    }
    value[camctl::find_key("gain_ceiling")] = 0x1f0;
    value[camctl::find_key("ae_target")] = 80;
    value[camctl::find_key("awb")] = 1;
    value[camctl::find_key("wb_r")] = V4L2_CID_RED_BALANCE_DEN;
    value[camctl::find_key("wb_b")] = V4L2_CID_BLUE_BALANCE_DEN;
    value[camctl::find_key("contrast")] = 128;
    value[camctl::find_key("saturation")] = 128;
    value[camctl::find_key("gamma")] = 100;
    value[camctl::find_key("sharpness")] = 100;
}

esp32p4_csi_device::image_ctl::~image_ctl()
{
    if (nullptr != sccb)
    {
        (void)i2c_master_bus_rm_device(sccb);
    }
    if (isp_fd >= 0)
    {
        ::close(isp_fd);
    }
}

void esp32p4_csi_device::image_ctl::attach_sensor(int port, int freq)
{
#if CONFIG_CAMERA_OV5647
    if (nullptr != sccb)
    {
        return;
    }
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(port), &bus) != ESP_OK ||
        nullptr == bus)
    {
        std::fprintf(stderr, "esp32p4_csi_camera_src: no I2C%d bus for image controls\n", port);
        return;
    }
    i2c_device_config_t dc{};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = k_ov5647_addr;
    dc.scl_speed_hz = static_cast<uint32_t>(freq);
    if (i2c_master_bus_add_device(bus, &dc, &sccb) != ESP_OK)
    {
        sccb = nullptr;
        std::fprintf(stderr, "esp32p4_csi_camera_src: SCCB device for image controls failed\n");
        return;
    }
    uint8_t hi = 0;
    uint8_t lo = 0;
    sensor_ok = sensor_read(0x300a, &hi) == 0 && sensor_read(0x300b, &lo) == 0 && 0x56 == hi &&
                0x47 == lo;
    if (!sensor_ok)
    {
        std::fprintf(stderr,
                     "esp32p4_csi_camera_src: chip id 0x%02x%02x is not OV5647; sensor "
                     "image controls off\n",
                     hi, lo);
    }
#else
    (void)port;
    (void)freq;
#endif
}

int esp32p4_csi_device::image_ctl::sensor_read(uint16_t reg, uint8_t *out)
{
    if (nullptr == sccb)
    {
        return -ENODEV;
    }
    const uint8_t a[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xff)};
    return i2c_master_transmit_receive(sccb, a, sizeof(a), out, 1, k_sccb_timeout_ms) == ESP_OK
               ? 0
               : -EIO;
}

int esp32p4_csi_device::image_ctl::sensor_write(uint16_t reg, uint8_t val)
{
    if (nullptr == sccb)
    {
        return -ENODEV;
    }
    const uint8_t b[3] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xff), val};
    return i2c_master_transmit(sccb, b, sizeof(b), k_sccb_timeout_ms) == ESP_OK ? 0 : -EIO;
}

int esp32p4_csi_device::image_ctl::sensor_bits(uint16_t reg, uint8_t mask, bool on)
{
    uint8_t cur = 0;
    if (const int r = sensor_read(reg, &cur); r < 0)
    {
        return r;
    }
    return sensor_write(reg, static_cast<uint8_t>(on ? (cur | mask) : (cur & ~mask)));
}

void esp32p4_csi_device::image_ctl::read_timing()
{
    uint8_t b[4] = {};
    if (sensor_read(0x380c, &b[0]) == 0 && sensor_read(0x380d, &b[1]) == 0 &&
        sensor_read(0x380e, &b[2]) == 0 && sensor_read(0x380f, &b[3]) == 0)
    {
        hts = (b[0] << 8) | b[1];
        vts = (b[2] << 8) | b[3];
    }
}

int esp32p4_csi_device::image_ctl::isp_open()
{
    if (isp_fd < 0)
    {
        isp_fd = ::open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
        if (isp_fd < 0)
        {
            std::fprintf(stderr, "esp32p4_csi_camera_src: open %s: %s\n",
                         ESP_VIDEO_ISP1_DEVICE_NAME, std::strerror(errno));
            return -ENODEV;
        }
    }
    return 0;
}

int esp32p4_csi_device::image_ctl::isp_set(uint32_t id, int32_t val)
{
    if (const int r = isp_open(); r < 0)
    {
        return r;
    }
    struct v4l2_ext_control c
    {
    };
    c.id = id;
    c.value = val;
    struct v4l2_ext_controls cs
    {
    };
    cs.ctrl_class = V4L2_CID_USER_CLASS;
    cs.count = 1;
    cs.controls = &c;
    return xioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &cs, "ISP S_EXT_CTRLS");
}

int esp32p4_csi_device::image_ctl::isp_set_struct(uint32_t id, void *p, size_t size)
{
    if (const int r = isp_open(); r < 0)
    {
        return r;
    }
    struct v4l2_ext_control c
    {
    };
    c.id = id;
    c.size = static_cast<uint32_t>(size);
    c.p_u8 = static_cast<uint8_t *>(p);
    struct v4l2_ext_controls cs
    {
    };
    cs.ctrl_class = V4L2_CID_USER_CLASS;
    cs.count = 1;
    cs.controls = &c;
    return xioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &cs, "ISP S_EXT_CTRLS");
}

int esp32p4_csi_device::image_ctl::isp_get_struct(uint32_t id, void *p, size_t size)
{
    if (const int r = isp_open(); r < 0)
    {
        return r;
    }
    struct v4l2_ext_control c
    {
    };
    c.id = id;
    c.size = static_cast<uint32_t>(size);
    c.p_u8 = static_cast<uint8_t *>(p);
    struct v4l2_ext_controls cs
    {
    };
    cs.ctrl_class = V4L2_CID_USER_CLASS;
    cs.count = 1;
    cs.controls = &c;
    return xioctl(isp_fd, VIDIOC_G_EXT_CTRLS, &cs, "ISP G_EXT_CTRLS");
}

/* Writes one key to the hardware. Caller holds mu and the stream is on. */
int esp32p4_csi_device::image_ctl::apply(int index)
{
    const std::string_view n = camctl::k_keys[index].name;
    const int val = value[index];
    if (camctl::target::sensor == camctl::k_keys[index].where)
    {
#if CONFIG_CAMERA_OV5647
        if (!sensor_ok)
        {
            return -ENODEV;
        }
        int r = 0;
        if (n == "exposure_us")
        {
            /* 0x3503 bit 0: manual AEC. Manual first: while AEC runs it owns
             * 0x3500..0x3502, and a value written then would be replaced by
             * the AEC's before manual mode froze it. */
            r |= sensor_bits(0x3503, 0x01, val > 0);
            if (val > 0)
            {
                const uint32_t e = camctl::ov5647_exposure_reg(val, hts, vts, k_ov5647_pclk_hz);
                r |= sensor_write(0x3500, static_cast<uint8_t>((e >> 16) & 0x0f));
                r |= sensor_write(0x3501, static_cast<uint8_t>((e >> 8) & 0xff));
                r |= sensor_write(0x3502, static_cast<uint8_t>(e & 0xff));
            }
        }
        else if (n == "gain")
        {
            /* 0x3503 bit 1: manual AGC, set first for the same reason;
             * 0x350a/0x350b: gain x16, 10 bits. */
            r |= sensor_bits(0x3503, 0x02, val > 0);
            if (val > 0)
            {
                r |= sensor_write(0x350a, static_cast<uint8_t>((val >> 8) & 0x03));
                r |= sensor_write(0x350b, static_cast<uint8_t>(val & 0xff));
            }
        }
        else if (n == "gain_ceiling")
        {
            r |= sensor_write(0x3a18, static_cast<uint8_t>((val >> 8) & 0x03));
            r |= sensor_write(0x3a19, static_cast<uint8_t>(val & 0xff));
        }
        else if (n == "ae_target")
        {
            const camctl::ae_target_regs t = camctl::ov5647_ae_target(val);
            r |= sensor_write(0x3a0f, t.high);
            r |= sensor_write(0x3a10, t.low);
            r |= sensor_write(0x3a1b, t.high);
            r |= sensor_write(0x3a1e, t.low);
            r |= sensor_write(0x3a11, t.fast_high);
            r |= sensor_write(0x3a1f, t.fast_low);
        }
        else if (n == "awb" || n == "wb_r" || n == "wb_b")
        {
            /* 0x5001 bit 0 enables the sensor's AWB gain block, as in the Linux
             * ov5647 driver; off, the raw output is unscaled. The OV5640-style
             * manual gains at 0x3400..0x3406 do not take on the OV5647 (0x3406
             * reads back 0 after a write), so manual gains are the ISP's
             * red / blue balance, which goes back to 1x when AWB is auto. */
            const bool manual = 0 == v("awb");
            r |= sensor_bits(0x5001, 0x01, !manual);
            if (r < 0)
            {
                return -EIO;
            }
            const int rg = manual ? v("wb_r") : V4L2_CID_RED_BALANCE_DEN;
            const int bg = manual ? v("wb_b") : V4L2_CID_BLUE_BALANCE_DEN;
            if (const int e = isp_set(V4L2_CID_RED_BALANCE, rg); e < 0)
            {
                return e;
            }
            return isp_set(V4L2_CID_BLUE_BALANCE, bg);
        }
        return r < 0 ? -EIO : 0;
#else
        return -ENOTSUP;
#endif
    }
    if (n == "brightness")
    {
        return isp_set(V4L2_CID_BRIGHTNESS, val);
    }
    if (n == "contrast")
    {
        return isp_set(V4L2_CID_CONTRAST, val);
    }
    if (n == "saturation")
    {
        return isp_set(V4L2_CID_SATURATION, val);
    }
    if (n == "hue")
    {
        return isp_set(V4L2_CID_HUE, val);
    }
    if (n == "gamma")
    {
        esp_video_isp_gamma_t g{};
        g.enable = val > 0;
        uint8_t x[camctl::k_gamma_points];
        uint8_t y[camctl::k_gamma_points];
        camctl::gamma_curve(val, x, y);
        for (int i = 0; i < camctl::k_gamma_points && i < ISP_GAMMA_CURVE_POINTS_NUM; ++i)
        {
            g.points[i].x = x[i];
            g.points[i].y = y[i];
        }
        return isp_set_struct(V4L2_CID_USER_ESP_ISP_GAMMA, &g, sizeof(g));
    }
    if (n == "sharpness")
    {
        if (!sharpen_base_valid)
        {
            /* The tuning's sharpen is the 100 % point. Read it once, before we
             * overwrite it; if the tuning has not run yet use the OV5647
             * tuning file's values. */
            esp_video_isp_sharpen_t cur{};
            if (isp_get_struct(V4L2_CID_USER_ESP_ISP_SHARPEN, &cur, sizeof(cur)) == 0 && cur.enable)
            {
                sharpen_base = cur;
            }
            else
            {
                sharpen_base = esp_video_isp_sharpen_t{};
                sharpen_base.h_thresh = 56;
                sharpen_base.l_thresh = 10;
                sharpen_base.h_coeff = 0.425f;
                sharpen_base.m_coeff = 0.625f;
                const uint8_t m[3][3] = {{1, 2, 1}, {2, 2, 2}, {1, 2, 1}};
                std::memcpy(sharpen_base.matrix, m, sizeof(m));
            }
            sharpen_base_valid = true;
        }
        esp_video_isp_sharpen_t s = sharpen_base;
        s.enable = val > 0;
        const float k = val / 100.0f;
        s.h_coeff = std::min(sharpen_base.h_coeff * k, k_sharpen_coeff_max);
        s.m_coeff = std::min(sharpen_base.m_coeff * k, k_sharpen_coeff_max);
        return isp_set_struct(V4L2_CID_USER_ESP_ISP_SHARPEN, &s, sizeof(s));
    }
    return -ENOTSUP;
}

/* Writes every key that was set. Caller holds mu and the stream is on. The AWB
 * group writes once, under "awb". */
void esp32p4_csi_device::image_ctl::apply_set(bool isp_only)
{
    bool awb_group = false;
    for (int i = 0; i < camctl::k_key_count; ++i)
    {
        const std::string_view n = camctl::k_keys[i].name;
        /* The AWB group ends in the ISP balance, which the tuning may write
         * too, so it is part of the ISP re-write as well. */
        const bool awb_key = n == "awb" || n == "wb_r" || n == "wb_b";
        if (!set[i] || (isp_only && !awb_key && camctl::target::isp != camctl::k_keys[i].where))
        {
            continue;
        }
        if (awb_key)
        {
            if (awb_group)
            {
                continue;
            }
            awb_group = true;
        }
        if (const int r = apply(i); r < 0)
        {
            std::fprintf(stderr, "esp32p4_csi_camera_src: %.*s %d not applied (%d)\n",
                         static_cast<int>(n.size()), n.data(), value[i], r);
        }
    }
}

int esp32p4_csi_device::image_ctl::sensor_state(std::string *out)
{
#if !CONFIG_CAMERA_OV5647
    (void)out;
    return -ENOTSUP;
#else
    if (!sensor_ok)
    {
        return -ENODEV;
    }
    uint8_t aec = 0, e0 = 0, e1 = 0, e2 = 0, g0 = 0, g1 = 0, wb = 0, c0 = 0, c1 = 0, t = 0;
    if (sensor_read(0x3503, &aec) < 0 || sensor_read(0x3500, &e0) < 0 ||
        sensor_read(0x3501, &e1) < 0 || sensor_read(0x3502, &e2) < 0 ||
        sensor_read(0x350a, &g0) < 0 || sensor_read(0x350b, &g1) < 0 ||
        sensor_read(0x5001, &wb) < 0 || sensor_read(0x3a18, &c0) < 0 ||
        sensor_read(0x3a19, &c1) < 0 || sensor_read(0x3a0f, &t) < 0)
    {
        return -EIO;
    }
    const uint32_t e = (static_cast<uint32_t>(e0 & 0x0f) << 16) | (e1 << 8) | e2;
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "aec=%s agc=%s exposure_us=%d exposure_lines=%" PRIu32
                  " gain=%d gain_ceiling=%d ae_high=%u awb=%s hts=%d vts=%d",
                  (aec & 0x01) ? "manual" : "auto", (aec & 0x02) ? "manual" : "auto",
                  camctl::ov5647_exposure_us(e, hts, k_ov5647_pclk_hz), e >> 4,
                  ((g0 & 0x03) << 8) | g1, ((c0 & 0x03) << 8) | c1, t,
                  (wb & 0x01) ? "auto" : "off", hts, vts);
    *out = buf;
    return 0;
#endif
}

int esp32p4_csi_device::image_ctl::isp_state(std::string *out)
{
    if (const int r = isp_open(); r < 0)
    {
        return r;
    }
    struct v4l2_ext_control c[6] = {};
    c[0].id = V4L2_CID_BRIGHTNESS;
    c[1].id = V4L2_CID_CONTRAST;
    c[2].id = V4L2_CID_SATURATION;
    c[3].id = V4L2_CID_HUE;
    c[4].id = V4L2_CID_RED_BALANCE;
    c[5].id = V4L2_CID_BLUE_BALANCE;
    struct v4l2_ext_controls cs
    {
    };
    cs.ctrl_class = V4L2_CID_USER_CLASS;
    cs.count = 6;
    cs.controls = c;
    if (const int r = xioctl(isp_fd, VIDIOC_G_EXT_CTRLS, &cs, "ISP G_EXT_CTRLS"); r < 0)
    {
        return r;
    }
    esp_video_isp_sharpen_t s{};
    const bool sh = isp_get_struct(V4L2_CID_USER_ESP_ISP_SHARPEN, &s, sizeof(s)) == 0;
    /* GAMMA_EXT: the plain GAMMA get logs a warning on every call. */
    esp_video_isp_gamma_ext_t g{};
    const bool gm = isp_get_struct(V4L2_CID_USER_ESP_ISP_GAMMA_EXT, &g, sizeof(g)) == 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "brightness=%" PRId32 " contrast=%" PRId32 " saturation=%" PRId32
                  " hue=%" PRId32 " wb_r=%" PRId32 " wb_b=%" PRId32
                  " sharpen=%s h_coeff=%.3f m_coeff=%.3f gamma=%s y16=%u y128=%u",
                  c[0].value, c[1].value, c[2].value, c[3].value, c[4].value, c[5].value,
                  !sh ? "?" : s.enable ? "on" : "off", static_cast<double>(s.h_coeff),
                  static_cast<double>(s.m_coeff), !gm ? "?" : g.enable ? "on" : "off",
                  gm ? g.green_points[0].y : 0U, gm ? g.green_points[7].y : 0U);
    *out = buf;
    return 0;
}

bool esp32p4_csi_device::live_key(std::string_view key)
{
    return camctl::find_key(key) >= 0;
}

esp32p4_csi_device::esp32p4_csi_device() : ctl_(std::make_unique<image_ctl>())
{
}

int esp32p4_csi_device::open()
{
    if (ring_)
    {
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(s_video_init_mu);
        if (!s_video_init_done)
        {
            esp_video_init_csi_config_t csi{};
            /* An I2C master bus the application already created on sccb_port
             * (other devices share the camera's bus) is used as is; esp_video
             * then neither creates nor deletes it, and sccb_scl / sccb_sda are
             * ignored. */
            i2c_master_bus_handle_t bus = nullptr;
            if (i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(sccb_port), &bus) ==
                    ESP_OK &&
                nullptr != bus)
            {
                csi.sccb_config.init_sccb = false;
                csi.sccb_config.i2c_handle = bus;
                std::printf("esp32p4_csi_camera_src: SCCB on the existing I2C%d bus\n",
                            sccb_port);
            }
            else
            {
                csi.sccb_config.init_sccb = true;
                csi.sccb_config.i2c_config.port = static_cast<uint8_t>(sccb_port);
                csi.sccb_config.i2c_config.scl_pin = static_cast<gpio_num_t>(sccb_scl);
                csi.sccb_config.i2c_config.sda_pin = static_cast<gpio_num_t>(sccb_sda);
            }
            csi.sccb_config.freq = static_cast<uint32_t>(sccb_freq);
            csi.reset_pin = static_cast<gpio_num_t>(reset_pin);
            csi.pwdn_pin = static_cast<gpio_num_t>(pwdn_pin);
            esp_video_init_config_t cfg{};
            cfg.csi = &csi;
            if (esp_video_init(&cfg) != ESP_OK)
            {
                std::fprintf(stderr,
                             "esp32p4_csi_camera_src: esp_video_init failed "
                             "(no sensor on SCCB "
                             "sda=%d scl=%d?)\n",
                             sccb_sda, sccb_scl);
                return -ENODEV;
            }
            s_video_init_done = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(ctl_->mu);
        ctl_->attach_sensor(sccb_port, sccb_freq);
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
    auto fail = [&](int err)
    {
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

    struct v4l2_capability cap
    {
    };
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
    {
        driver = reinterpret_cast<const char *>(cap.driver);
    }
#if CONFIG_CAMERA_OV5647
    {
        std::lock_guard<std::mutex> lock(ctl_->mu);
        ov5647_fix_format(fd, fps, light_freq, rotate, ctl_->v("gain_ceiling"));
    }
#endif

    struct v4l2_format fmt
    {
    };
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
    /* esp_video's YUV420 is the ISP's packed O_UYY_E_VYY, which the P4 H.264
     * core reads. */
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
        std::fprintf(stderr,
                     "esp32p4_csi_camera_src: sensor mode is %" PRIu32 "x%" PRIu32
                     ", not YUV420 %dx%d\n",
                     fmt.fmt.pix.width, fmt.fmt.pix.height, width, height);
        return fail(-EINVAL);
    }
    live_w = static_cast<int>(fmt.fmt.pix.width);
    live_h = static_cast<int>(fmt.fmt.pix.height);

    struct v4l2_streamparm parm
    {
    };
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0)
    {
        const uint32_t num = parm.parm.capture.timeperframe.numerator;
        const uint32_t den = parm.parm.capture.timeperframe.denominator;
        const float driver_fps = (num > 0 && den > 0) ? static_cast<float>(den) / num : 0.f;
        std::fprintf(stderr,
                     "esp32p4_csi_camera_src: driver timeperframe %" PRIu32 "/%" PRIu32
                     " (~%.2f fps) cap=0x%" PRIx32 "\n",
                     num, den, static_cast<double>(driver_fps),
                     static_cast<uint32_t>(parm.parm.capture.capability));
        if (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)
        {
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = fps > 0 ? static_cast<uint32_t>(fps) : 30U;
            (void)xioctl(fd, VIDIOC_S_PARM, &parm, "S_PARM fps");
        }
    }
    else
    {
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps > 0 ? static_cast<uint32_t>(fps) : 30U;
        (void)xioctl(fd, VIDIOC_S_PARM, &parm, "S_PARM fps");
    }

    struct v4l2_requestbuffers req
    {
    };
    req.count = static_cast<uint32_t>(nbufs);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    err = xioctl(fd, VIDIOC_REQBUFS, &req, "REQBUFS");
    if (err < 0)
    {
        return fail(err);
    }
    if (req.count < k_min_bufs)
    {
        return fail(-ENOMEM);
    }
    r->ptr.assign(req.count, nullptr);
    r->len.assign(req.count, 0);
    for (uint32_t i = 0; i < req.count; ++i)
    {
        struct v4l2_buffer b
        {
        };
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

    struct timeval tv
    {
    };
    tv.tv_usec = k_dqbuf_timeout_ms * 1000;
    err = xioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &tv, "S_DQBUF_TIMEOUT");
    if (err < 0)
    {
        return fail(err);
    }
    for (uint32_t i = 0; i < req.count; ++i)
    {
        struct v4l2_buffer b
        {
        };
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
    {
        /* The sensor table and the driver's AE target load at S_FMT; the keys
         * set so far go on top. */
        std::lock_guard<std::mutex> lock(ctl_->mu);
        ctl_->read_timing();
        ctl_->apply_set(false);
        ctl_->frames = 0;
        ctl_->reapply_at = k_isp_reapply_frame;
    }
    return 0;
}

void esp32p4_csi_device::release_ring(std::shared_ptr<ring> r)
{
    std::unique_lock<std::mutex> lock(r->mu);
    r->streaming = false;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)ioctl(r->fd, VIDIOC_STREAMOFF, &type);
    if (!r->cv.wait_for(lock, k_release_wait, [&] { return r->held <= 0; }))
    {
        /* Closing the device frees the buffers; a consumer still reads one.
         * Leak instead. */
        std::fprintf(stderr,
                     "esp32p4_csi_camera_src: %d frame(s) still held "
                     "downstream; leaving %s "
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

esp32p4_csi_device::~esp32p4_csi_device()
{
    close();
}

void esp32p4_csi_device::close()
{
    if (ring_)
    {
        release_ring(std::move(ring_));
    }
}

int esp32p4_csi_device::dequeue(component_pdu &out)
{
    auto r = ring_;
    if (!r)
    {
        return -EBADF;
    }
    const size_t want = frame_bytes(live_w, live_h);
    struct v4l2_buffer b
    {
    };
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    const int64_t dq_start_ns = steady_mono_ns();
    if (ioctl(r->fd, VIDIOC_DQBUF, &b) != 0)
    {
        const int e = errno;
        return e == ETIMEDOUT || e == EAGAIN || e == EINTR ? -EAGAIN : -(e ? e : EIO);
    }
    /* esp_video leaves the V4L2 timestamp at 0. */
    const uint64_t ts_us = static_cast<uint64_t>(steady_mono_ns() / 1000);
    {
        const uint64_t wait_us = ts_us - static_cast<uint64_t>(dq_start_ns / 1000);
        ++dq_count;
        dq_wait_us_sum += wait_us;
        if (wait_us < 1000)
        {
            ++dq_ready;
        }
        /* Finished frames still queued in the driver (V4L2_BUF_FLAG_DONE). */
        uint32_t done = 0;
        for (uint32_t i = 0; i < r->ptr.size(); ++i)
        {
            struct v4l2_buffer q
            {
            };
            q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            q.memory = V4L2_MEMORY_MMAP;
            q.index = i;
            if (i != b.index && ioctl(r->fd, VIDIOC_QUERYBUF, &q) == 0 &&
                (q.flags & V4L2_BUF_FLAG_DONE))
            {
                ++done;
            }
        }
        dq_done_sum += done;
        uint32_t max = dq_done_max.load();
        while (done > max && !dq_done_max.compare_exchange_weak(max, done))
        {
        }
    }
    if (b.index >= r->ptr.size())
    {
        return -EIO;
    }
    {
        std::lock_guard<std::mutex> lock(r->mu);
        ++r->held;
    }
    /* Short or errored frame (bytesused 0): straight back to the driver. */
    if (b.bytesused != want)
    {
        r->requeue(b.index);
        return -EIO;
    }

    if (0 != ctl_->reapply_at && ++ctl_->frames >= ctl_->reapply_at)
    {
        ctl_->reapply_at = 0;
        std::lock_guard<std::mutex> lock(ctl_->mu);
        ctl_->apply_set(true);
    }

    /* No cache sync: the CPU never reads the pixels and the H.264 DMA reads
     * PSRAM. */
    const uint32_t index = b.index;

    component_pdu pdu;
    pdu.ts_us = ts_us;
    pdu.sdu_type = sdu_type_e::O_UYY_E_VYY;
    pdu.port = 0;
    pdu.sdu = bfc::shared_sized_buffer::adopt(reinterpret_cast<std::byte *>(r->ptr[index]),
                                              r->len[index], b.bytesused,
                                              [r, index](std::byte *) { r->requeue(index); });

    out = std::move(pdu);
    return 0;
}

int esp32p4_csi_device::configure(std::string_view key, std::string_view value)
{
    if (key == "fps")
    {
        return parse_int(value, 1, 120, &fps);
    }
    if (const int i = camctl::find_key(key); i >= 0)
    {
        int64_t n = 0;
        if (key_parse_i64(value, &n) < 0 || !camctl::value_ok(i, n))
        {
            return -EINVAL;
        }
        std::lock_guard<std::mutex> lock(ctl_->mu);
        ctl_->value[i] = static_cast<int>(n);
        ctl_->set[i] = true;
        return ring_ ? ctl_->apply(i) : 0;
    }
    if (ring_)
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
    if (key == "rotate")
    {
        int r = 0;
        if (parse_int(value, 0, 180, &r) < 0 || (0 != r && 180 != r))
        {
            return -EINVAL;
        }
        rotate = r;
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

int esp32p4_csi_device::query(std::string_view key, std::string *value) const
{
    if (!value)
    {
        return -EINVAL;
    }
    if (key == "size")
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dx%d", ring_ ? live_w : width, ring_ ? live_h : height);
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
    if (key == "dq_stats")
    {
        const uint32_t n = dq_count.exchange(0);
        const uint32_t ready = dq_ready.exchange(0);
        const uint32_t done = dq_done_sum.exchange(0);
        const uint32_t done_max = dq_done_max.exchange(0);
        const uint64_t wait = dq_wait_us_sum.exchange(0);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "dq=%" PRIu32 " ready=%" PRIu32 " wait_avg_ms=%.2f done_avg=%.2f done_max=%" PRIu32,
                      n, ready, n ? wait / 1000.0 / n : 0.0, n ? double(done) / n : 0.0, done_max);
        *value = buf;
        return 0;
    }
    if (key == "light_freq")
    {
        return set_int(value, light_freq);
    }
    /* The value last set (or the default); sensor_state / isp_state read the
     * hardware back. */
    if (const int i = camctl::find_key(key); i >= 0)
    {
        std::lock_guard<std::mutex> lock(ctl_->mu);
        return set_int(value, ctl_->value[i]);
    }
    if (key == "sensor_state" || key == "isp_state")
    {
        if (!ring_)
        {
            return -EAGAIN;
        }
        std::lock_guard<std::mutex> lock(ctl_->mu);
        return key == "sensor_state" ? ctl_->sensor_state(value) : ctl_->isp_state(value);
    }
    if (key == "rotate")
    {
        return set_int(value, rotate);
    }
    if (key == "driver")
    {
        *value = driver;
        return 0;
    }
    if (key == "sccb_port")
    {
        return set_int(value, sccb_port);
    }
    if (key == "sccb_scl")
    {
        return set_int(value, sccb_scl);
    }
    if (key == "sccb_sda")
    {
        return set_int(value, sccb_sda);
    }
    if (key == "sccb_freq")
    {
        return set_int(value, sccb_freq);
    }
    if (key == "reset_pin")
    {
        return set_int(value, reset_pin);
    }
    if (key == "pwdn_pin")
    {
        return set_int(value, pwdn_pin);
    }
    return -ENOTSUP;
}
}  // namespace vstreamer::detail
