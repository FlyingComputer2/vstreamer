#include "core/rs_encode_scalar.hpp"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <vector>

#if defined(ESP_PLATFORM) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
/* CONFIG_VSTREAMER_RS_PIE (idf/vstreamer/Kconfig) also requires SOC_CPU_HAS_PIE. */
#if defined(CONFIG_VSTREAMER_RS_PIE) && CONFIG_VSTREAMER_RS_PIE && defined(__riscv)
#define VSTREAMER_RS_PIE 1
#include "esp_heap_caps.h"
#include "soc/soc.h"
#endif

namespace vstreamer
{
namespace
{

/* out[i] (^)= c * in[i] for the coefficient c whose nibble table is t. The 256-entry product
 * table costs 256 steps per call, against one lookup per byte instead of two plus shifts; with
 * both rows word aligned, four bytes share one load and one store of in and out. */
template <bool accumulate>
void mul_row(const uint8_t *t, const uint8_t *in, uint8_t *out, int len)
{
    uint8_t full[256];
    for (int h = 0; h < 16; h++)
    {
        for (int l = 0; l < 16; l++)
        {
            full[(h << 4) | l] = static_cast<uint8_t>(t[l] ^ t[16 + h]);
        }
    }
    int i = 0;
    if (0 == ((reinterpret_cast<uintptr_t>(in) | reinterpret_cast<uintptr_t>(out)) & 3U))
    {
        const auto *in4 = reinterpret_cast<const uint32_t *>(in);
        auto       *out4 = reinterpret_cast<uint32_t *>(out);
        for (; i + 4 <= len; i += 4)
        {
            const uint32_t x = in4[i / 4];
            const uint32_t p = static_cast<uint32_t>(full[x & 0xffU]) |
                               (static_cast<uint32_t>(full[(x >> 8) & 0xffU]) << 8) |
                               (static_cast<uint32_t>(full[(x >> 16) & 0xffU]) << 16) |
                               (static_cast<uint32_t>(full[x >> 24]) << 24);
            out4[i / 4] = accumulate ? out4[i / 4] ^ p : p;
        }
    }
    for (; i < len; i++)
    {
        const uint8_t p = full[in[i]];
        out[i] = accumulate ? static_cast<uint8_t>(out[i] ^ p) : p;
    }
}

#ifdef VSTREAMER_RS_PIE

/* ESP32-P4 PIE (128-bit q registers; no byte shuffle, so no nibble-table multiply). The product
 * c * x is the xor over the set bits b of x of (c << b). Per 16-byte chunk each source is
 * expanded once into 8 byte masks E_m = 0xff where bit (7 - m) of x is CLEAR (and + vcmp.eq
 * against zero; PIE adds saturate, so there is no cheap shift), shared by every parity row. A
 * row then xors (E_m & broadcast(c << (7 - m))) over all sources and bits, which counts every
 * clear bit instead of every set one; starting the accumulator at xor_j (c_j * 255), the sum of
 * all eight shifted c, turns that back into the product. Each per-row step is 4 PIE ops.
 * The PIE load/store forms only encode x8-x15 and x24-x31 as the address, so the pointers are
 * pinned to a2-a5 (GCC has no constraint for that set). */

/* Not const: keeps it in RAM (.data) rather than flash rodata, for the PIE loads. */
alignas(16) uint8_t k_bit_const[8][16] = {
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80},
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40},
    {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10},
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02},
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
};

#define PIE_MASK                                                                            \
    "esp.vld.128.ip q2, %[b], 16\n esp.andq q1, q0, q2\n"                                  \
    "esp.vcmp.eq.u8 q1, q1, q7\n esp.vst.128.ip q1, %[m], 16\n"

inline void pie_masks(const uint8_t *src16, uint8_t *masks)
{
    register uint8_t       *m asm("a2") = masks;
    register const uint8_t *bc asm("a3") = &k_bit_const[0][0];
    register const uint8_t *s asm("a4") = src16;
    asm volatile(
        "esp.zero.q q7\n"
        "esp.vld.128.ip q0, %[s], 0\n" PIE_MASK PIE_MASK PIE_MASK PIE_MASK PIE_MASK PIE_MASK
            PIE_MASK PIE_MASK
        : [m] "+r"(m), [b] "+r"(bc)
        : [s] "r"(s)
        : "memory");
}

#define PIE_STEP_A                                                                          \
    "esp.vld.128.ip q1, %[m], 16\n esp.vldbc.8.ip q2, %[c], 1\n"                             \
    "esp.andq q1, q1, q2\n esp.xorq q0, q0, q1\n"
#define PIE_STEP_B                                                                          \
    "esp.vld.128.ip q3, %[m], 16\n esp.vldbc.8.ip q4, %[c], 1\n"                             \
    "esp.andq q3, q3, q4\n esp.xorq q6, q6, q3\n"

/* dst16 (16-byte aligned) = xor over the k sources of c * x for one chunk. */
inline void pie_row(const uint8_t *masks, const uint8_t *coefs, const uint8_t *base, int k,
                    uint8_t *dst16)
{
    register const uint8_t *m asm("a2") = masks;
    register const uint8_t *c asm("a3") = coefs;
    register uint8_t       *d asm("a4") = dst16;
    register const uint8_t *k0 asm("a5") = base;
    int                     n = k;
    asm volatile(
        "esp.vldbc.8.ip q0, %[k0], 0\n esp.zero.q q6\n"
        "1:\n" PIE_STEP_A PIE_STEP_B PIE_STEP_A PIE_STEP_B PIE_STEP_A PIE_STEP_B PIE_STEP_A
            PIE_STEP_B "addi %[n], %[n], -1\n bnez %[n], 1b\n"
        "esp.xorq q0, q0, q6\n"
        "esp.vst.128.ip q0, %[d], 0\n"
        : [m] "+r"(m), [c] "+r"(c), [n] "+r"(n), [d] "+r"(d)
        : [k0] "r"(k0)
        : "memory");
}

/* PIE's 128-bit loads and stores fault on RTCRAM and SPM, which plain malloc also hands out
 * (MALLOC_CAP_SIMD excludes them); only aligned L2MEM and PSRAM addresses go to PIE directly,
 * anything else is bounced through the scratch. */
bool pie_direct(const void *p)
{
    const auto a = reinterpret_cast<uintptr_t>(p);
    return 0 == (a & 15U) && ((a >= SOC_DRAM_LOW && a < SOC_DRAM_HIGH) ||
                              (a >= SOC_EXTRAM_LOW && a < SOC_EXTRAM_HIGH));
}

/* Scratch is heap (MALLOC_CAP_SIMD), not stack: the encoder also runs on small-stack threads (a
 * k/n change on the M-plane thread flushes the pending block), and the tables are up to ~6 KiB.
 * False when the shape is too small or the allocation fails (caller falls back). */
bool rs_encode_pie(int len, int k, int rows, const unsigned char *g_tbls,
                   unsigned char *const *data, unsigned char *const *coding)
{
    if (k < 1 || rows < 1 || len < 16)
    {
        return false;
    }
    const size_t coef_bytes = static_cast<size_t>(rows) * static_cast<size_t>(k) * 8;
    const size_t mask_bytes = static_cast<size_t>(k) * 128;
    /* masks | tmp (16) | coefs | base (rows); masks and tmp need 16-byte alignment. */
    auto *scratch = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, mask_bytes + 16 + coef_bytes + rows, MALLOC_CAP_SIMD));
    if (nullptr == scratch)
    {
        return false;
    }
    uint8_t *masks = scratch;
    uint8_t *tmp = masks + mask_bytes;
    /* coefs[r][j][m] = c(r,j) << (7 - m), straight from the nibble table. */
    uint8_t *coefs = tmp + 16;
    for (int rj = 0; rj < rows * k; rj++)
    {
        const uint8_t *t = g_tbls + static_cast<size_t>(rj) * 32;
        uint8_t       *o = coefs + rj * 8;
        o[0] = t[16 + 8];
        o[1] = t[16 + 4];
        o[2] = t[16 + 2];
        o[3] = t[16 + 1];
        o[4] = t[8];
        o[5] = t[4];
        o[6] = t[2];
        o[7] = t[1];
    }
    uint8_t *base = coefs + coef_bytes;  // per row: xor over sources of c * 255
    for (int r = 0; r < rows; r++)
    {
        uint8_t v = 0;
        for (int i = 0; i < k * 8; i++)
        {
            v = static_cast<uint8_t>(v ^ coefs[r * k * 8 + i]);
        }
        base[r] = v;
    }
    const int chunks = len / 16;
    for (int ch = 0; ch < chunks; ch++)
    {
        const int off = ch * 16;
        for (int j = 0; j < k; j++)
        {
            const uint8_t *src = data[j] + off;
            if (!pie_direct(src))
            {
                std::memcpy(tmp, src, 16);
                src = tmp;
            }
            pie_masks(src, masks + j * 128);
        }
        for (int r = 0; r < rows; r++)
        {
            uint8_t *dst = coding[r] + off;
            if (!pie_direct(dst))
            {
                pie_row(masks, coefs + r * k * 8, base + r, k, tmp);
                std::memcpy(dst, tmp, 16);
            }
            else
            {
                pie_row(masks, coefs + r * k * 8, base + r, k, dst);
            }
        }
    }
    for (int i = chunks * 16; i < len; i++)
    {
        for (int r = 0; r < rows; r++)
        {
            const uint8_t *row_tbls = g_tbls + static_cast<size_t>(r) * static_cast<size_t>(k) * 32;
            uint8_t        acc = 0;
            for (int j = 0; j < k; j++)
            {
                const uint8_t *t = row_tbls + static_cast<size_t>(j) * 32;
                const uint8_t  x = data[j][i];
                acc = static_cast<uint8_t>(acc ^ t[x & 15U] ^ t[16 + (x >> 4)]);
            }
            coding[r][i] = acc;
        }
    }
    heap_caps_free(scratch);
    return true;
}

#endif  // VSTREAMER_RS_PIE

}  // namespace

void rs_encode_scalar_table(int len, int k, int rows, const unsigned char *g_tbls,
                            unsigned char *const *data, unsigned char *const *coding)
{
    for (int r = 0; r < rows; r++)
    {
        const uint8_t *row_tbls = g_tbls + static_cast<size_t>(r) * static_cast<size_t>(k) * 32;
        uint8_t       *out = coding[r];
        for (int j = 0; j < k; j++)
        {
            if (0 == j)
            {
                mul_row<false>(row_tbls, data[j], out, len);
            }
            else
            {
                mul_row<true>(row_tbls + static_cast<size_t>(j) * 32, data[j], out, len);
            }
        }
    }
}


#ifdef VSTREAMER_RS_PIE
namespace
{
/* Cleared by rs_encode_selftest() when the PIE kernel disagrees with the table path. */
std::atomic<bool> g_pie_enabled {true};
}  // namespace
#endif

void rs_encode_scalar(int len, int k, int rows, const unsigned char *g_tbls,
                      unsigned char *const *data, unsigned char *const *coding)
{
#ifdef VSTREAMER_RS_PIE
    if (g_pie_enabled.load(std::memory_order_relaxed) && rs_encode_pie(len, k, rows, g_tbls, data, coding))
    {
        return;
    }
#endif
    rs_encode_scalar_table(len, k, rows, g_tbls, data, coding);
}

const char *rs_encode_scalar_impl()
{
#ifdef VSTREAMER_RS_PIE
    return g_pie_enabled.load(std::memory_order_relaxed) ? "pie" : "table";
#else
    return "table";
#endif
}


namespace
{
uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++)
    {
        if (b & 1U)
        {
            p = static_cast<uint8_t>(p ^ a);
        }
        const bool hi = (a & 0x80U) != 0;
        a = static_cast<uint8_t>(a << 1);
        if (hi)
        {
            a = static_cast<uint8_t>(a ^ 0x1dU);
        }
        b = static_cast<uint8_t>(b >> 1);
    }
    return p;
}
}  // namespace

int rs_encode_selftest(int k, int rows, int len, int iterations, int64_t (*now_us)(), int64_t *us_fast,
                       int64_t *us_table, uint8_t *arena)
{
    std::vector<uint8_t> tbl(static_cast<size_t>(k * rows) * 32);
    uint32_t             rng = 12345;
    auto                 next = [&rng]() { return static_cast<uint8_t>((rng = rng * 1664525U + 1013904223U) >> 24); };
    for (int rj = 0; rj < k * rows; rj++)
    {
        const uint8_t c = static_cast<uint8_t>(next() | 1U);
        for (int i = 0; i < 16; i++)
        {
            tbl[static_cast<size_t>(rj) * 32 + i] = gf_mul(c, static_cast<uint8_t>(i));
            tbl[static_cast<size_t>(rj) * 32 + 16 + i] = gf_mul(c, static_cast<uint8_t>(i << 4));
        }
    }
    /* odd offsets/lengths on purpose: the kernel must cope with any alignment and tail. */
    const size_t         stride = static_cast<size_t>(len) + 8;
    std::vector<uint8_t> own;
    if (nullptr == arena)
    {
        own.resize(static_cast<size_t>(k + 2 * rows) * stride);
        arena = own.data();
    }
    std::vector<uint8_t *> sp, ap, bp;
    for (int i = 0; i < k + 2 * rows; i++)
    {
        uint8_t *row = arena + static_cast<size_t>(i) * stride;
        auto    &dst = i < k ? sp : (i < k + rows ? ap : bp);
        dst.push_back(row + (i & 3));
        if (i < k)
        {
            for (size_t x = 0; x < stride; x++)
            {
                row[x] = next();
            }
        }
    }
    const int64_t t0 = now_us();
    for (int i = 0; i < iterations; i++)
    {
        rs_encode_scalar(len, k, rows, tbl.data(), sp.data(), ap.data());
    }
    const int64_t t1 = now_us();
    for (int i = 0; i < iterations; i++)
    {
        rs_encode_scalar_table(len, k, rows, tbl.data(), sp.data(), bp.data());
    }
    const int64_t t2 = now_us();
    *us_fast = (t1 - t0) / iterations;
    *us_table = (t2 - t1) / iterations;
    int bad = 0;
    for (int r = 0; r < rows; r++)
    {
        bad += std::memcmp(ap[static_cast<size_t>(r)], bp[static_cast<size_t>(r)], static_cast<size_t>(len)) != 0;
    }
#ifdef VSTREAMER_RS_PIE
    if (bad > 0)
    {
        /* Wrong parity would make every receiver rebuild lost packets wrongly, with nothing in
         * the stats to show it: fall back to the table path for the rest of the run. */
        g_pie_enabled.store(false, std::memory_order_relaxed);
    }
#endif
    return bad;
}

}  // namespace vstreamer
