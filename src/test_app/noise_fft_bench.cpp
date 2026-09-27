#include "components/noise_source.hpp"

#include "core/data_packet.hpp"
#include "core/noise_fft2.hpp"

#include <pffft/pffft.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{

double bench_ms(int reps, auto fn)
{
    fn();
    fn();
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    for (int i = 0; i < reps; i++)
    {
        fn();
    }
    const auto t1 = clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(reps);
}

int set_str(vstreamer::noise_source &src, const char *key, std::string *storage, const char *val)
{
    *storage = val;
    std::string_view sv = *storage;
    return src.configure(key, &sv);
}

}  // namespace

int main()
{
    std::printf("machine: RK3588-class (aarch64)\n");
    std::printf("pffft: %s simd_width=%d\n", pffft_simd_arch(), pffft_simd_size());

    vstreamer::noise_fft2_plan fft;
    uint32_t                   rng = 1;

    struct case_s
    {
        int w;
        int h;
        int bw;
    };
    const case_s plane_cases[] = {
        {2, 2, 15},
        {52, 30, 25},
        {160, 90, 50},
        {320, 180, 75},
        {640, 360, 50},
        {1280, 720, 30},
        {1920, 1080, 30},
        {1920, 1080, 50},
        {1920, 1080, 75},
    };

    std::printf("\n[synthesize_plane] one luma plane, ms/frame:\n");
    std::printf("%10s %4s %8s\n", "size", "bw", "ms");
    for (const case_s &c : plane_cases)
    {
        std::vector<uint8_t> buf(static_cast<size_t>(c.w) * static_cast<size_t>(c.h));
        const int reps = (c.w * c.h > 400000) ? 25 : (c.w * c.h > 50000) ? 100 : 250;
        const double ms = bench_ms(reps,
                                   [&]
                                   {
                                       fft.synthesize_plane(buf.data(), c.w, c.h, c.bw, &rng);
                                   });
        std::printf("%4dx%-4d %4d %8.3f  (%6.1f fps if only this)\n", c.w, c.h, c.bw, ms,
                    1000.0 / ms);
    }

    vstreamer::noise_source src;
    std::string             cfg;
    src.open();

    const case_s nv12_cases[] = {
        {320, 240, 100},
        {320, 240, 25},
        {1280, 720, 100},
        {1280, 720, 50},
        {1280, 720, 15},
        {1920, 1080, 100},
        {1920, 1080, 50},
        {1920, 1080, 25},
        {1920, 1080, 15},
    };

    std::printf("\n[noise_source::output] full NV12 (Y+UV IFFT or fast path), ms/frame:\n");
    std::printf("%10s %4s %8s\n", "size", "bw", "ms");
    for (const case_s &c : nv12_cases)
    {
        if (set_str(src, "size", &cfg, (std::to_string(c.w) + "x" + std::to_string(c.h)).c_str()) <
            0)
        {
            continue;
        }
        if (set_str(src, "noise-bandwidth", &cfg, std::to_string(c.bw).c_str()) < 0)
        {
            continue;
        }

        const int reps = (c.w * c.h > 500000) ? 20 : 80;
        const double ms =
            bench_ms(reps,
                     [&]
                     {
                         vstreamer::data_packet pkt;
                         if (src.output(0, pkt, 0) == 0)
                         {
                             pkt.release();
                         }
                     });
        std::printf("%4dx%-4d %4d %8.3f  (%6.1f fps)\n", c.w, c.h, c.bw, ms, 1000.0 / ms);
    }

    return 0;
}
