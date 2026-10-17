#include "core/rs_encode_scalar.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(ESP_PLATFORM) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#if defined(CONFIG_SOC_CPU_HAS_PIE) && CONFIG_SOC_CPU_HAS_PIE && defined(__riscv)
#define VSTREAMER_RS_PIE 1
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

constexpr int k_pie_max_k = 31;
constexpr int k_pie_max_coef = 240;  // rows * k with k + rows <= 31

/* ESP32-P4 PIE (128-bit q registers; no byte shuffle, so no nibble-table multiply). The product
 * c * x is the xor over the set bits b of x of (c << b). Per 16-byte chunk each source is
 * expanded once into 8 byte masks E_m = 0xff where bit (7 - m) of x is CLEAR (and + vcmp.eq
 * against zero; PIE adds saturate, so there is no cheap shift), shared by every parity row. A
 * row then xors (E_m & broadcast(c << (7 - m))) over all sources and bits, which counts every
 * clear bit instead of every set one; starting the accumulator at xor_j (c_j * 255), the sum of
 * all eight shifted c, turns that back into the product. Each per-row step is 4 PIE ops. */

alignas(16) const uint8_t k_bit_const[8][16] = {
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
    const uint8_t *bc = &k_bit_const[0][0];
    asm volatile(
        "esp.zero.q q7\n"
        "esp.vld.128.ip q0, %[s], 0\n" PIE_MASK PIE_MASK PIE_MASK PIE_MASK PIE_MASK PIE_MASK
            PIE_MASK PIE_MASK
        : [m] "+r"(masks), [b] "+r"(bc)
        : [s] "r"(src16)
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
    const uint8_t *m = masks;
    const uint8_t *c = coefs;
    int            n = k;
    uint8_t       *d = dst16;
    asm volatile(
        "esp.vldbc.8.ip q0, %[k0], 0\n esp.zero.q q6\n"
        "1:\n" PIE_STEP_A PIE_STEP_B PIE_STEP_A PIE_STEP_B PIE_STEP_A PIE_STEP_B PIE_STEP_A
            PIE_STEP_B "addi %[n], %[n], -1\n bnez %[n], 1b\n"
        "esp.xorq q0, q0, q6\n"
        "esp.vst.128.ip q0, %[d], 0\n"
        : [m] "+r"(m), [c] "+r"(c), [n] "+r"(n), [d] "+r"(d)
        : [k0] "r"(base)
        : "memory");
}

/* False when the shape is outside what the on-stack tables hold (caller falls back). */
bool rs_encode_pie(int len, int k, int rows, const unsigned char *g_tbls,
                   unsigned char *const *data, unsigned char *const *coding)
{
    if (k < 1 || k > k_pie_max_k || rows < 1 || rows * k > k_pie_max_coef || len < 16)
    {
        return false;
    }
    /* coefs[r][j][m] = c(r,j) << (7 - m), straight from the nibble table. */
    alignas(16) uint8_t coefs[k_pie_max_coef * 8];
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
    uint8_t base[k_pie_max_coef];  // per row: xor over sources of c * 255
    for (int r = 0; r < rows; r++)
    {
        uint8_t v = 0;
        for (int i = 0; i < k * 8; i++)
        {
            v = static_cast<uint8_t>(v ^ coefs[r * k * 8 + i]);
        }
        base[r] = v;
    }
    alignas(16) uint8_t masks[k_pie_max_k * 128];
    alignas(16) uint8_t tmp[16];
    const int           chunks = len / 16;
    for (int ch = 0; ch < chunks; ch++)
    {
        const int off = ch * 16;
        for (int j = 0; j < k; j++)
        {
            const uint8_t *src = data[j] + off;
            if (reinterpret_cast<uintptr_t>(src) & 15U)
            {
                std::memcpy(tmp, src, 16);
                src = tmp;
            }
            pie_masks(src, masks + j * 128);
        }
        for (int r = 0; r < rows; r++)
        {
            uint8_t *dst = coding[r] + off;
            if (reinterpret_cast<uintptr_t>(dst) & 15U)
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


void rs_encode_scalar(int len, int k, int rows, const unsigned char *g_tbls,
                      unsigned char *const *data, unsigned char *const *coding)
{
#ifdef VSTREAMER_RS_PIE
    if (rs_encode_pie(len, k, rows, g_tbls, data, coding))
    {
        return;
    }
#endif
    rs_encode_scalar_table(len, k, rows, g_tbls, data, coding);
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
                       int64_t *us_table)
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
    std::vector<std::vector<uint8_t>> src(static_cast<size_t>(k), std::vector<uint8_t>(static_cast<size_t>(len) + 8));
    std::vector<std::vector<uint8_t>> a(static_cast<size_t>(rows), std::vector<uint8_t>(static_cast<size_t>(len) + 8));
    std::vector<std::vector<uint8_t>> b(a);
    std::vector<uint8_t *>            sp, ap, bp;
    int                               shift = 0;
    for (auto &v : src)
    {
        for (auto &x : v)
        {
            x = next();
        }
        sp.push_back(v.data() + (shift++ & 3));
    }
    for (auto &v : a)
    {
        ap.push_back(v.data() + (shift++ & 3));
    }
    for (auto &v : b)
    {
        bp.push_back(v.data() + (shift++ & 3));
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
    return bad;
}

}  // namespace vstreamer
