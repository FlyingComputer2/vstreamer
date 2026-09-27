#include "core/noise_fft2.hpp"

#include <pffft/pffft.h>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

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

float rng_unit(uint32_t *state)
{
    return (static_cast<float>(rng32(state)) / static_cast<float>(UINT32_MAX)) * 2.f - 1.f;
}

int fft_dim_for(int logical)
{
    const int min_n = pffft_min_fft_size(PFFFT_COMPLEX);
    const int want = std::max(logical, min_n);
    if (pffft_is_valid_size(want, PFFFT_COMPLEX))
    {
        return want;
    }
    return pffft_nearest_transform_size(want, PFFFT_COMPLEX, 1);
}

int omp_thread_count()
{
#ifdef _OPENMP
    return std::max(1, omp_get_max_threads());
#else
    return 1;
#endif
}

float axis_passband_mask(float coord, float half_len, int bandwidth)
{
    if (bandwidth <= 0)
    {
        return 0.f;
    }
    if (bandwidth >= 100)
    {
        return 1.f;
    }

    const float limit = (static_cast<float>(bandwidth) / 100.f) * half_len;
    if (limit <= 0.f || coord >= limit)
    {
        return 0.f;
    }

    const float edge = std::max(1.f, 0.08f * limit);
    const float flat = limit - edge;
    if (coord <= flat)
    {
        return 1.f;
    }

    const float t = (coord - flat) / edge;
    return 0.5f * (1.f + std::cos(static_cast<float>(M_PI) * t));
}

float passband_mask(int x, int y, int fw, int fh, int bandwidth)
{
    const float bx = static_cast<float>(x <= fw / 2 ? x : fw - x);
    const float by = static_cast<float>(y <= fh / 2 ? y : fh - y);
    const float r = std::hypot(bx, by);
    const float half_r =
        std::min(static_cast<float>(fw) / 2.f, static_cast<float>(fh) / 2.f);
    return axis_passband_mask(r, half_r, bandwidth);
}

void fill_hermitian_spectrum_row(float *spec, int fw, int fh, int y, int bandwidth,
                                 uint32_t *rng, float bin_scale)
{
    auto put = [&](int yy, int x, float re, float im)
    {
        float *p = spec + 2U * (static_cast<size_t>(yy) * static_cast<size_t>(fw) +
                                static_cast<size_t>(x));
        p[0] = re;
        p[1] = im;
    };

    const int x_max = (y == 0 || y == fh / 2) ? fw / 2 : fw - 1;
    for (int x = 0; x <= x_max; x++)
    {
        const float m = passband_mask(x, y, fw, fh, bandwidth);
        if (m <= 0.f)
        {
            continue;
        }

        const bool nyquist_bin = (x == 0 || x == fw / 2) && (y == 0 || y == fh / 2);
        const bool dc_bin = (x == 0 && y == 0);
        float        re = 0.f;
        float        im = 0.f;
        if (!dc_bin)
        {
            if (nyquist_bin)
            {
                re = rng_unit(rng) * m * bin_scale;
            }
            else
            {
                re = rng_unit(rng) * m * bin_scale;
                im = rng_unit(rng) * m * bin_scale;
            }
        }

        put(y, x, re, im);
        const int y2 = (y == 0) ? 0 : fh - y;
        const int x2 = (x == 0) ? 0 : fw - x;
        if (y2 != y || x2 != x)
        {
            put(y2, x2, re, -im);
        }
    }
}

void fill_hermitian_spectrum(float *spec, int fw, int fh, int bandwidth, uint32_t *rng)
{
    std::memset(spec, 0, static_cast<size_t>(fw) * static_cast<size_t>(fh) * 2U * sizeof(float));

    const float bin_scale = std::sqrt(static_cast<float>(fw) * static_cast<float>(fh));

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y <= fh / 2; y++)
    {
        uint32_t row_rng = *rng ^ (static_cast<uint32_t>(y) * 747796405u);
        fill_hermitian_spectrum_row(spec, fw, fh, y, bandwidth, &row_rng, bin_scale);
    }
    rng32(rng);
}

void transpose_complex(const float *src, float *dst, int fw, int fh)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < fh; y++)
    {
        for (int x = 0; x < fw; x++)
        {
            const size_t si = 2U * (static_cast<size_t>(y) * static_cast<size_t>(fw) +
                                    static_cast<size_t>(x));
            const size_t di = 2U * (static_cast<size_t>(x) * static_cast<size_t>(fh) +
                                    static_cast<size_t>(y));
            dst[di] = src[si];
            dst[di + 1U] = src[si + 1U];
        }
    }
}

void ifft_rows_parallel(PFFFT_Setup *setup, float *grid, int fw, int fh, float *thread_work,
                        int work_stride)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < fh; y++)
    {
        float *row = grid + 2U * static_cast<size_t>(y) * static_cast<size_t>(fw);
#ifdef _OPENMP
        float *work = thread_work + static_cast<size_t>(omp_get_thread_num()) *
                                        static_cast<size_t>(work_stride);
#else
        float *work = thread_work;
#endif
        pffft_transform_ordered(setup, row, row, work, PFFFT_BACKWARD);
    }
}

void ifft_2d(PFFFT_Setup *setup_w, PFFFT_Setup *setup_h, float *grid, float *grid_t, int fw,
             int fh, float *thread_work, int work_stride)
{
    ifft_rows_parallel(setup_w, grid, fw, fh, thread_work, work_stride);
    transpose_complex(grid, grid_t, fw, fh);
    ifft_rows_parallel(setup_h, grid_t, fh, fw, thread_work, work_stride);
    transpose_complex(grid_t, grid, fh, fw);
}

float spatial_sample_norm(const float *spec, int fw, int x, int y, float norm)
{
    const float *p =
        spec + 2U * (static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x));
    return p[0] * norm;
}

void spectrum_to_uint8_plane(const float *spec, int fw, int fh, int crop_w, int crop_h,
                             uint8_t *out, int bandwidth)
{
    (void)bandwidth;
    const float norm = 1.f / (static_cast<float>(fw) * static_cast<float>(fh));
    const size_t plane_sz = static_cast<size_t>(crop_w) * static_cast<size_t>(crop_h);

    float peak_abs = 0.f;
#ifdef _OPENMP
#pragma omp parallel for reduction(max : peak_abs) schedule(static)
#endif
    for (int y = 0; y < crop_h; y++)
    {
        for (int x = 0; x < crop_w; x++)
        {
            const float av =
                std::fabs(spatial_sample_norm(spec, fw, x, y, norm));
            peak_abs = std::max(peak_abs, av);
        }
    }

    if (peak_abs <= 1e-9f)
    {
        std::memset(out, 128, plane_sz);
        return;
    }

    const float gain = 127.f / peak_abs;

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int y = 0; y < crop_h; y++)
    {
        uint8_t *row = out + static_cast<size_t>(y) * static_cast<size_t>(crop_w);
        for (int x = 0; x < crop_w; x++)
        {
            const float v = spatial_sample_norm(spec, fw, x, y, norm);
            int         q = 128 + static_cast<int>(std::lround(v * gain));
            if (q < 0)
            {
                q = 0;
            }
            else if (q > 255)
            {
                q = 255;
            }
            row[x] = static_cast<uint8_t>(q);
        }
    }
}

}  // namespace

void noise_fft2_configure_threads_from_env()
{
#ifdef _OPENMP
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       omp_set_max_active_levels(2);
                       const char *env = std::getenv("VSTREAMER_NOISE_THREADS");
                       if (nullptr == env || env[0] == '\0')
                       {
                           return;
                       }
                       char      *end = nullptr;
                       const long n = std::strtol(env, &end, 10);
                       if (end == env || n < 1 || n > 64)
                       {
                           return;
                       }
                       omp_set_num_threads(static_cast<int>(n));
                   });
#endif
}

struct noise_fft2_plan::impl
{
    int fw = 0;
    int fh = 0;

    PFFFT_Setup *setup_w = nullptr;
    PFFFT_Setup *setup_h = nullptr;

    float *grid = nullptr;
    float *grid_t = nullptr;
    std::vector<float> thread_work;
    int                work_stride = 0;

    void release()
    {
        if (setup_w != nullptr)
        {
            pffft_destroy_setup(setup_w);
            setup_w = nullptr;
        }
        if (setup_h != nullptr)
        {
            pffft_destroy_setup(setup_h);
            setup_h = nullptr;
        }
        pffft_aligned_free(grid);
        grid = nullptr;
        pffft_aligned_free(grid_t);
        grid_t = nullptr;
        thread_work.clear();
        work_stride = 0;
        fw = 0;
        fh = 0;
    }

    int ensure(int iw, int ih)
    {
        const int need_w = fft_dim_for(iw);
        const int need_h = fft_dim_for(ih);
        if (need_w == fw && need_h == fh && setup_w != nullptr && setup_h != nullptr)
        {
            return 0;
        }
        release();

        setup_w = pffft_new_setup(need_w, PFFFT_COMPLEX);
        setup_h = pffft_new_setup(need_h, PFFFT_COMPLEX);
        if (setup_w == nullptr || setup_h == nullptr)
        {
            release();
            return -ENOMEM;
        }

        const size_t grid_floats = 2U * static_cast<size_t>(need_w) * static_cast<size_t>(need_h);
        grid = static_cast<float *>(pffft_aligned_malloc(grid_floats * sizeof(float)));
        grid_t = static_cast<float *>(pffft_aligned_malloc(grid_floats * sizeof(float)));
        if (grid == nullptr || grid_t == nullptr)
        {
            release();
            return -ENOMEM;
        }

        work_stride = 2 * std::max(need_w, need_h);
        const int    nthr = omp_thread_count();
        thread_work.resize(static_cast<size_t>(work_stride) * static_cast<size_t>(nthr));

        fw = need_w;
        fh = need_h;
        return 0;
    }
};

noise_fft2_plan::noise_fft2_plan() = default;

noise_fft2_plan::~noise_fft2_plan()
{
    reset();
}

void noise_fft2_plan::reset()
{
    if (state != nullptr)
    {
        state->release();
        delete state;
        state = nullptr;
    }
}

const char *noise_fft2_plan::simd_arch() const
{
    return pffft_simd_arch();
}

int noise_fft2_plan::last_fft_w() const
{
    return state != nullptr ? state->fw : 0;
}

int noise_fft2_plan::last_fft_h() const
{
    return state != nullptr ? state->fh : 0;
}

int noise_fft2_plan::synthesize_plane(uint8_t *out, int iw, int ih, int bandwidth, uint32_t *rng)
{
    noise_fft2_configure_threads_from_env();
    if (out == nullptr || iw < 2 || ih < 2 || rng == nullptr)
    {
        return -EINVAL;
    }
    if (bandwidth <= 0)
    {
        std::memset(out, 128, static_cast<size_t>(iw) * static_cast<size_t>(ih));
        return 0;
    }

    if (state == nullptr)
    {
        state = new (std::nothrow) impl();
        if (state == nullptr)
        {
            return -ENOMEM;
        }
    }

    const int r = state->ensure(iw, ih);
    if (r < 0)
    {
        return r;
    }

    fill_hermitian_spectrum(state->grid, state->fw, state->fh, bandwidth, rng);
    ifft_2d(state->setup_w, state->setup_h, state->grid, state->grid_t, state->fw, state->fh,
            state->thread_work.data(), state->work_stride);
    spectrum_to_uint8_plane(state->grid, state->fw, state->fh, iw, ih, out, bandwidth);
    return 0;
}

}  // namespace vstreamer
