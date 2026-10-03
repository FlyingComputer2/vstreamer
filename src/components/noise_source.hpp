#ifndef VSTREAMER_SOURCE_NOISE_SOURCE_HPP
#define VSTREAMER_SOURCE_NOISE_SOURCE_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_NOISE_SOURCE
#error "noise_source requires -DENABLE_NOISE_SOURCE=ON"
#endif

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

#include "core/component_source.hpp"
#include "core/noise_fft2.hpp"

namespace vstreamer
{

/* Synthetic NV12: bandwidth-shaped spectrum → SIMD IFFT (PFFFT). Default 416x240@30. */
class noise_source : public component_source
{
public:
    noise_source();
    ~noise_source() override;

    noise_source(const noise_source &) = delete;
    noise_source &operator=(const noise_source &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    void stop_pregenerate();

    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    int  fill_nv12_locked(uint8_t *dst, size_t dst_sz);
    void pace_unlocked(int fps, int timeout_ms);
    void invalidate_pregenerated_locked();
    int  ensure_pregenerated_locked();
    void join_pregenerate_worker();
    void kick_pregenerate_async_locked();

    std::atomic<bool> pregen_cancel {false};

    static constexpr int k_pregenerate_max = 128;

    mutable std::mutex mu;
    bool               opened = false;

    int width = 416;
    int height = 240;
    int fps = 30;
    /* Spatial bandwidth 0..100 (0 = flat gray, 100 = full-rate noise). */
    int noise_bandwidth = 100;
    int noise_block_size = 0;
    int pregenerate_count = 0;

    int64_t pts = 0;
    double  due_sec = 0;
    uint32_t rng = 1;

    std::vector<uint8_t> scratch_chroma;
    std::vector<std::vector<uint8_t>> pregenerated;
    bool                              pregen_ready = false;
    int                               pregen_w = 0;
    int                               pregen_h = 0;
    int                               pregen_bandwidth = 0;
    size_t                            pregen_cursor = 0;
    std::atomic<bool> pregenerating {false};
    std::atomic<int>  pregen_build_n {0};
    std::atomic<int>  pregen_build_total {0};

    std::thread pregen_worker;

    noise_fft2_plan fft;
    noise_fft2_plan fft_chroma_u;
    noise_fft2_plan fft_chroma_v;
};

}  // namespace vstreamer

#endif  // VSTREAMER_SOURCE_NOISE_SOURCE_HPP
