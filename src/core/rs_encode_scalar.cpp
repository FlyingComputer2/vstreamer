#include "core/rs_encode_scalar.hpp"

#include <cstddef>
#include <cstdint>

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

}  // namespace

void rs_encode_scalar(int len, int k, int rows, const unsigned char *g_tbls,
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

}  // namespace vstreamer
