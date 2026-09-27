#ifndef VSTREAMER_CORE_NOISE_FFT2_HPP
#define VSTREAMER_CORE_NOISE_FFT2_HPP

#include <cstdint>

namespace vstreamer
{

/* Once per process: `VSTREAMER_NOISE_THREADS` (1..64) for OpenMP in noise IFFT; unset = default. */
void noise_fft2_configure_threads_from_env();

/* Bandwidth-shaped spectrum (low-pass disk from DC) → 2D IFFT → 8-bit plane (PFFFT / NEON). */
struct noise_fft2_plan
{
    noise_fft2_plan();
    ~noise_fft2_plan();

    noise_fft2_plan(const noise_fft2_plan &) = delete;
    noise_fft2_plan &operator=(const noise_fft2_plan &) = delete;

    /* Synthesize iw×ih samples into out (row-major, linesize=iw). Returns 0 or -errno. */
    int synthesize_plane(uint8_t *out, int iw, int ih, int bandwidth, uint32_t *rng);

    void reset();

    [[nodiscard]] const char *simd_arch() const;
    [[nodiscard]] int         last_fft_w() const;
    [[nodiscard]] int         last_fft_h() const;

private:
    struct impl;
    impl *state = nullptr;
};

}  // namespace vstreamer

#endif
