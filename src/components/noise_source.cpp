#include "components/noise_source.hpp"

#include "core/time_util.hpp"

#include "core/key_util.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace vstreamer
{
namespace
{

uint32_t rng32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x != 0 ? x : 1;
    return *state;
}

double now_sec()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void interleave_uv_from_planes(const uint8_t *u, const uint8_t *v, int luma_w, int chroma_h,
                               uint8_t *uv)
{
    const int cw = luma_w / 2;
    for (int y = 0; y < chroma_h; y++)
    {
        const uint8_t *urow = u + static_cast<size_t>(y) * static_cast<size_t>(cw);
        const uint8_t *vrow = v + static_cast<size_t>(y) * static_cast<size_t>(cw);
        uint8_t       *drow = uv + static_cast<size_t>(y) * static_cast<size_t>(luma_w);
        for (int x = 0; x < cw; x++)
        {
            drow[2 * x] = urow[x];
            drow[2 * x + 1] = vrow[x];
        }
    }
}

int fill_nv12_chroma_ifft(noise_fft2_plan *fft_u, noise_fft2_plan *fft_v, uint8_t *uv, int luma_w,
                          int luma_h, int bandwidth, uint32_t *rng_u, uint32_t *rng_v,
                          std::vector<uint8_t> &tmp)
{
    const int chroma_h = luma_h / 2;
    const int cw = luma_w / 2;
    const size_t plane_sz = static_cast<size_t>(cw) * static_cast<size_t>(chroma_h);
    tmp.resize(plane_sz * 2U);
    uint8_t *u = tmp.data();
    uint8_t *v = u + plane_sz;

    int r_u = 0;
    int r_v = 0;
    std::thread tu(
        [&]
        {
            r_u = fft_u->synthesize_plane(u, cw, chroma_h, bandwidth, rng_u);
        });
    std::thread tv(
        [&]
        {
            r_v = fft_v->synthesize_plane(v, cw, chroma_h, bandwidth, rng_v);
        });
    tu.join();
    tv.join();

    if (r_u < 0)
    {
        return r_u;
    }
    if (r_v < 0)
    {
        return r_v;
    }
    interleave_uv_from_planes(u, v, luma_w, chroma_h, uv);
    return 0;
}

int parse_noise_bandwidth(std::string_view v, int *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    int64_t n = 0;
    std::string tmp(v);
    if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 0 || n > 100)
    {
        return -EINVAL;
    }
    *out = static_cast<int>(n);
    return 0;
}

int parse_noise_block_size(std::string_view v, int *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    int64_t n = 0;
    std::string tmp(v);
    if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 0 || n > 256)
    {
        return -EINVAL;
    }
    *out = static_cast<int>(n);
    return 0;
}

bool key_is_noise_bandwidth(std::string_view key)
{
    return key == "noise-bandwidth" || key == "noise_bandwidth" || key == "noise-randomness" ||
           key == "noise_randomness" || key == "randomness";
}

bool key_is_noise_block_size(std::string_view key)
{
    return key == "noise-block-size" || key == "noise_block_size";
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
    if (ww < 2 || hh < 2 || (ww % 2) || (hh % 2) || ww > 7680 || hh > 4320)
    {
        return -EINVAL;
    }
    *w = static_cast<int>(ww);
    *h = static_cast<int>(hh);
    return 0;
}

}  // namespace

noise_source::noise_source() = default;

noise_source::~noise_source()
{
    close();
}

void noise_source::join_pregenerate_worker()
{
    if (pregen_worker.joinable())
    {
        pregen_worker.join();
    }
}

void noise_source::stop_pregenerate()
{
    pregen_cancel.store(true, std::memory_order_release);
    join_pregenerate_worker();
    pregen_cancel.store(false, std::memory_order_release);
}

void noise_source::kick_pregenerate_async_locked()
{
    if (pregenerate_count <= 0 || pregen_ready || pregen_worker.joinable())
    {
        return;
    }
    pregen_worker = std::thread(
        [this]
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!opened || pregenerate_count <= 0)
            {
                return;
            }
            (void)ensure_pregenerated_locked();
        });
}

std::string noise_source::name() const
{
    return "noise";
}

media_kind_e noise_source::output_kind() const
{
    return media_kind_e::NV12;
}

int noise_source::open()
{
    std::lock_guard<std::mutex> lock(mu);
    if (opened)
    {
        return 0;
    }
    pts = 0;
    due_sec = 0;
    opened = true;
    kick_pregenerate_async_locked();
    return 0;
}

void noise_source::close()
{
    stop_pregenerate();
    std::lock_guard<std::mutex> lock(mu);
    opened = false;
    due_sec = 0;
    scratch_chroma.clear();
    invalidate_pregenerated_locked();
    fft.reset();
    fft_chroma_u.reset();
    fft_chroma_v.reset();
}

void noise_source::invalidate_pregenerated_locked()
{
    pregenerated.clear();
    pregen_ready = false;
    pregen_w = 0;
    pregen_h = 0;
    pregen_bandwidth = 0;
    pregen_cursor = 0;
    pregenerating.store(false, std::memory_order_relaxed);
    pregen_build_n.store(0, std::memory_order_relaxed);
    pregen_build_total.store(0, std::memory_order_relaxed);
    if (pregenerate_count > 0)
    {
        pregen_build_total.store(pregenerate_count, std::memory_order_release);
    }
}

int noise_source::ensure_pregenerated_locked()
{
    if (pregenerate_count <= 0)
    {
        return 0;
    }
    if (pregen_ready && pregen_w == width && pregen_h == height &&
        pregen_bandwidth == noise_bandwidth &&
        pregenerated.size() == static_cast<size_t>(pregenerate_count))
    {
        return 0;
    }

    invalidate_pregenerated_locked();
    const size_t nv12_sz =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3U / 2U;
    pregenerated.resize(static_cast<size_t>(pregenerate_count));
    pregen_build_total.store(pregenerate_count, std::memory_order_release);
    pregen_build_n.store(1, std::memory_order_release);
    pregenerating.store(true, std::memory_order_release);
    std::fprintf(stderr, "noise_source: pregenerating %d NV12 frame(s) %dx%d bandwidth=%d\n",
                 pregenerate_count, width, height, noise_bandwidth);
    for (int i = 0; i < pregenerate_count; i++)
    {
        if (pregen_cancel.load(std::memory_order_acquire))
        {
            invalidate_pregenerated_locked();
            return -ECANCELED;
        }
        pregen_build_n.store(i + 1, std::memory_order_release);
        pregenerated[static_cast<size_t>(i)].resize(nv12_sz);
        const int r =
            fill_nv12_locked(pregenerated[static_cast<size_t>(i)].data(), nv12_sz);
        if (r < 0)
        {
            invalidate_pregenerated_locked();
            return r;
        }
    }
    pregenerating.store(false, std::memory_order_release);
    pregen_build_n.store(0, std::memory_order_release);
    pregen_build_total.store(0, std::memory_order_release);
    pregen_ready = true;
    pregen_w = width;
    pregen_h = height;
    pregen_bandwidth = noise_bandwidth;
    pregen_cursor = 0;
    std::fprintf(stderr, "noise_source: pregenerate loop ready (%zu bytes/frame)\n", nv12_sz);
    return 0;
}

int noise_source::fill_nv12_locked(uint8_t *dst, size_t dst_sz)
{
    if (width < 2 || height < 2 || (width % 2) != 0 || (height % 2) != 0)
    {
        return -EINVAL;
    }

    const size_t y_sz = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t nv12_sz = y_sz + y_sz / 2ULL;
    if (dst_sz < nv12_sz || nullptr == dst)
    {
        return -EINVAL;
    }

    if (noise_bandwidth <= 0)
    {
        std::memset(dst, 128, nv12_sz);
        return 0;
    }

    uint32_t rng_luma = rng;
    uint32_t rng_u = rng32(&rng);
    uint32_t rng_v = rng32(&rng);

    int r_y = 0;
    int r_c = 0;
#ifdef _OPENMP
#pragma omp parallel sections
    {
#pragma omp section
        {
            r_y = fft.synthesize_plane(dst, width, height, noise_bandwidth, &rng_luma);
        }
#pragma omp section
        {
            r_c = fill_nv12_chroma_ifft(&fft_chroma_u, &fft_chroma_v, dst + y_sz, width, height,
                                        noise_bandwidth, &rng_u, &rng_v, scratch_chroma);
        }
    }
#else
    r_y = fft.synthesize_plane(dst, width, height, noise_bandwidth, &rng_luma);
    r_c = fill_nv12_chroma_ifft(&fft_chroma_u, &fft_chroma_v, dst + y_sz, width, height,
                                noise_bandwidth, &rng_u, &rng_v, scratch_chroma);
#endif
    rng ^= rng_luma ^ rng_u ^ rng_v;

    if (r_y < 0)
    {
        return r_y;
    }
    return r_c;
}

void noise_source::pace_unlocked(int fps_val, int timeout_ms)
{
    int fps = fps_val > 0 ? fps_val : 30;
    double period = 1.0 / static_cast<double>(fps);
    double t0 = now_sec();
    if (due_sec <= 0)
    {
        due_sec = t0 + period;
    }
    else
    {
        due_sec += period;
    }

    const double now = now_sec();
    double       wait = due_sec - now;
    if (wait < -1.0)
    {
        due_sec = now + period;
        wait = period;
    }
    if (wait <= 0)
    {
        return;
    }
    if (timeout_ms == 0)
    {
        return;
    }
    if (timeout_ms > 0)
    {
        double cap = static_cast<double>(timeout_ms) / 1000.0;
        if (wait > cap)
        {
            wait = cap;
        }
    }
    std::this_thread::sleep_for(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<double>(wait)));
}

int noise_source::output(uint8_t /*port*/, data_packet &out, int timeout_ms)
{
    join_pregenerate_worker();

    int     fps_val = 0;
    int     w = 0;
    int     h = 0;
    int64_t frame_pts = 0;

    {
        std::lock_guard<std::mutex> lock(mu);
        if (!opened)
        {
            return -EBADF;
        }

        fps_val = fps;
        w = width;
        h = height;
        frame_pts = pts;
        pts++;

        const size_t y_sz = static_cast<size_t>(w) * static_cast<size_t>(h);
        const size_t nv12_sz = y_sz + y_sz / 2ULL;

        int r = ensure_pregenerated_locked();
        if (r < 0)
        {
            pts--;
            return r;
        }

        auto *buf = static_cast<uint8_t *>(std::malloc(nv12_sz));
        if (nullptr == buf)
        {
            pts--;
            return -ENOMEM;
        }

        if (pregenerate_count > 0 && pregen_ready)
        {
            const size_t idx = pregen_cursor % pregenerated.size();
            pregen_cursor++;
            std::memcpy(buf, pregenerated[idx].data(), nv12_sz);
        }
        else
        {
            r = fill_nv12_locked(buf, nv12_sz);
            if (r < 0)
            {
                std::free(buf);
                pts--;
                return r;
            }
        }

        auto fd = std::make_unique<frame_data>();
        fd->kind = media_kind_e::NV12;
        fd->width = w;
        fd->height = h;
        fd->pts = frame_pts;
        fd->capture_mono_ns = steady_mono_ns();
        fd->key = true;
        fd->buf = shared_sized_buffer::adopt(reinterpret_cast<std::byte *>(buf), nv12_sz, nv12_sz,
                                             [](std::byte *p) {
                                                 std::free(reinterpret_cast<uint8_t *>(p));
                                             });
        out.reset(std::move(fd));
    }

    pace_unlocked(fps_val, timeout_ms);
    return 0;
}

int noise_source::configure(std::string_view key, std::string_view value)
{
    std::string_view v = value;

    join_pregenerate_worker();
    std::lock_guard<std::mutex> lock(mu);
    if (key == "size")
    {
        int w = 0;
        int h = 0;
        int r = parse_size(v, &w, &h);
        if (r < 0)
        {
            return r;
        }
        width = w;
        height = h;
        invalidate_pregenerated_locked();
        return 0;
    }
    if (key == "fps")
    {
        int64_t n = 0;
        std::string tmp(v);
        int r = key_parse_i64(tmp.c_str(), &n);
        if (r < 0 || n <= 0 || n > 240)
        {
            return -EINVAL;
        }
        fps = static_cast<int>(n);
        return 0;
    }
    if (key == "format")
    {
        if (v == "nv12" || v == "NV12")
        {
            return 0;
        }
        return -EINVAL;
    }
    if (key_is_noise_bandwidth(key))
    {
        int r = 0;
        int pr = parse_noise_bandwidth(v, &r);
        if (pr < 0)
        {
            return pr;
        }
        noise_bandwidth = r;
        invalidate_pregenerated_locked();
        return 0;
    }
    if (key == "pregenerate-frames" || key == "pregenerate-frame" ||
        key == "pregenerate_frames" || key == "pregenerate_frame")
    {
        int64_t n = 0;
        std::string tmp(v);
        if (key_parse_i64(tmp.c_str(), &n) < 0 || n < 0 || n > k_pregenerate_max)
        {
            return -EINVAL;
        }
        pregenerate_count = static_cast<int>(n);
        invalidate_pregenerated_locked();
        return 0;
    }
    if (key_is_noise_block_size(key))
    {
        int r = 0;
        int pr = parse_noise_block_size(v, &r);
        if (pr < 0)
        {
            return pr;
        }
        noise_block_size = r;
        return 0;
    }
    return -ENOTSUP;
}

int noise_source::query(std::string_view key, std::string *value) const
{
    if (key == "state")
    {
        const int n = pregen_build_n.load(std::memory_order_acquire);
        const int total = pregen_build_total.load(std::memory_order_acquire);
        thread_local std::string state_out;
        if (total > 0)
        {
            const int shown = n > 0 ? n : 1;
            char      buf[48];
            if (std::snprintf(buf, sizeof(buf), "pregeneration_%d/%d", shown, total) < 0)
            {
                return -EINVAL;
            }
            state_out = buf;
        }
        else
        {
            state_out = "running";
        }
        *value = state_out;
        return 0;
    }

    if (nullptr == value)
    {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lock(mu);
    if (key == "status")
    {
        *value = "noise_nv12";
        return 0;
    }
    if (key == "size")
    {
        char buf[64];
        if (std::snprintf(buf, sizeof(buf), "%dx%d", width, height) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "fps")
    {
        char buf[32];
        if (key_format_i64(fps, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "format")
    {
        *value = "nv12";
        return 0;
    }
    if (key_is_noise_bandwidth(key))
    {
        char buf[16];
        if (key_format_i64(noise_bandwidth, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "pregenerate-frames" || key == "pregenerate-frame" ||
        key == "pregenerate_frames" || key == "pregenerate_frame")
    {
        char buf[16];
        if (key_format_i64(pregenerate_count, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "noise-fft-grid" || key == "noise_fft_grid")
    {
        char buf[32];
        if (std::snprintf(buf, sizeof(buf), "%dx%d", fft.last_fft_w(), fft.last_fft_h()) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "noise-internal-size" || key == "noise_internal_size")
    {
        char buf[32];
        if (std::snprintf(buf, sizeof(buf), "%dx%d", width, height) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "noise-luma-block-size" || key == "noise_luma_block_size")
    {
        *value = "1";
        return 0;
    }
    if (key == "noise-fft-simd" || key == "noise_fft_simd")
    {
        *value = fft.simd_arch();
        return 0;
    }
    if (key_is_noise_block_size(key))
    {
        char buf[16];
        if (key_format_i64(noise_block_size, buf, sizeof(buf)) < 0)
        {
            return -EINVAL;
        }
        *value = buf;
        return 0;
    }
    if (key == "device")
    {
        *value = "noise";
        return 0;
    }
    if (key == "media_type")
    {
        *value = "raw";
        return 0;
    }
    if (key == "pixel_type")
    {
        *value = "nv12";
        return 0;
    }
    if (key == "width")
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", width);
        *value = buf;
        return 0;
    }
    if (key == "height")
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", height);
        *value = buf;
        return 0;
    }
    return -ENOTSUP;
}

}  // namespace vstreamer
