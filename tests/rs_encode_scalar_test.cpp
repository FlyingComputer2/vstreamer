#include "core/rs_encode_scalar.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

extern "C"
{
#include "erasure_code.h"
}

namespace
{

struct encode_case
{
    int k;
    int rows;
    int len;
    std::vector<std::vector<uint8_t>> data;
    std::vector<uint8_t>              g_tbls;
    std::vector<uint8_t *>            data_ptrs;
};

encode_case make_case(int k, int n, int len, uint32_t seed)
{
    encode_case c;
    c.k = k;
    c.rows = n - k;
    c.len = len;
    std::mt19937 rng(seed);
    c.data.assign(static_cast<size_t>(k), std::vector<uint8_t>(static_cast<size_t>(len)));
    for (auto &row : c.data)
    {
        for (auto &b : row)
        {
            b = static_cast<uint8_t>(rng());
        }
        c.data_ptrs.push_back(row.data());
    }
    std::vector<uint8_t> matrix(static_cast<size_t>(n * k));
    gf_gen_cauchy1_matrix(matrix.data(), n, k);
    c.g_tbls.resize(static_cast<size_t>(k * c.rows * 32));
    ec_init_tables_base(k, c.rows, matrix.data() + k * k, c.g_tbls.data());
    return c;
}

std::vector<std::vector<uint8_t>> encode(encode_case &c, bool scalar)
{
    std::vector<std::vector<uint8_t>> out(static_cast<size_t>(c.rows),
                                          std::vector<uint8_t>(static_cast<size_t>(c.len), 0xa5));
    std::vector<uint8_t *> ptrs;
    for (auto &row : out)
    {
        ptrs.push_back(row.data());
    }
    if (scalar)
    {
        vstreamer::rs_encode_scalar(c.len, c.k, c.rows, c.g_tbls.data(), c.data_ptrs.data(),
                                    ptrs.data());
    }
    else
    {
        ec_encode_data_base(c.len, c.k, c.rows, c.g_tbls.data(), c.data_ptrs.data(),
                            ptrs.data());
    }
    return out;
}

}  // namespace

TEST(RsEncodeScalarTest, MatchesIsalBase)
{
    const int shapes[][3] = {{1, 1, 16}, {1, 2, 17}, {4, 6, 100}, {10, 12, 1400},
                             {21, 31, 1401}, {27, 31, 64}, {16, 31, 1}, {30, 31, 333}};
    uint32_t seed = 1;
    for (const auto &s : shapes)
    {
        encode_case c = make_case(s[0], s[1], s[2], seed++);
        EXPECT_EQ(encode(c, false), encode(c, true)) << "k=" << s[0] << " n=" << s[1]
                                                     << " len=" << s[2];
    }
}

/* Decode passes rows that start inside received datagrams: odd offsets take the byte path. */
TEST(RsEncodeScalarTest, MatchesIsalBaseUnaligned)
{
    encode_case c = make_case(10, 14, 1403, 99);
    for (int off = 1; off < 4; off++)
    {
        std::vector<std::vector<uint8_t>> src(10, std::vector<uint8_t>(1403 + 4));
        std::vector<uint8_t *>            ptrs;
        for (size_t j = 0; j < src.size(); j++)
        {
            std::copy(c.data[j].begin(), c.data[j].end(), src[j].begin() + off);
            ptrs.push_back(src[j].data() + off);
        }
        std::vector<std::vector<uint8_t>> base(4, std::vector<uint8_t>(1403 + 4));
        std::vector<std::vector<uint8_t>> mine(4, std::vector<uint8_t>(1403 + 4));
        std::vector<uint8_t *>            bp;
        std::vector<uint8_t *>            mp;
        for (int r = 0; r < 4; r++)
        {
            bp.push_back(base[static_cast<size_t>(r)].data() + off);
            mp.push_back(mine[static_cast<size_t>(r)].data() + (off ^ 1));
        }
        ec_encode_data_base(1403, 10, 4, c.g_tbls.data(), ptrs.data(), bp.data());
        vstreamer::rs_encode_scalar(1403, 10, 4, c.g_tbls.data(), ptrs.data(), mp.data());
        for (int r = 0; r < 4; r++)
        {
            EXPECT_TRUE(std::equal(bp[static_cast<size_t>(r)], bp[static_cast<size_t>(r)] + 1403,
                                   mp[static_cast<size_t>(r)]))
                << "off=" << off << " row=" << r;
        }
    }
}

/* Reports the speedup; asserts only that the scalar path is not slower. */
TEST(RsEncodeScalarTest, FasterThanIsalBase)
{
    encode_case c = make_case(21, 31, 1400, 7);
    const auto  time_it = [&c](bool scalar)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; i++)
        {
            (void)encode(c, scalar);
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    (void)time_it(true);
    const double base_ms = time_it(false);
    const double scalar_ms = time_it(true);
    std::printf("21/31 x1400 B x50: isa-l base %.1f ms, scalar %.1f ms (%.1fx)\n", base_ms,
                scalar_ms, base_ms / scalar_ms);
    EXPECT_LT(scalar_ms, base_ms);
}
