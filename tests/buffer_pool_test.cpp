#include "core/buffer_pool.hpp"

#include <gtest/gtest.h>

#include <vector>

using vstreamer::buffer_pool;
using vstreamer::shared_sized_buffer;

TEST(BufferPoolTest, PoolBufferOutlivesPool)
{
    shared_sized_buffer buf;
    {
        buffer_pool pool(64, 2);
        buf = pool.acquire(32);
        EXPECT_FALSE(buf.empty());
        EXPECT_EQ(buf.size(), 32u);
    }
    buf.clear();
}

TEST(BufferPoolTest, PoolRecycles)
{
    constexpr size_t k_depth = 4;
    buffer_pool pool(128, k_depth);
    EXPECT_EQ(pool.misses(), 0u);

    std::vector<shared_sized_buffer> held;
    for (size_t i = 0; i < k_depth; i++)
    {
        shared_sized_buffer b = pool.acquire(64);
        EXPECT_FALSE(b.empty());
        held.push_back(std::move(b));
    }
    EXPECT_EQ(pool.misses(), 0u);

    shared_sized_buffer extra = pool.acquire(64);
    EXPECT_FALSE(extra.empty());
    EXPECT_EQ(pool.misses(), 1u);

    held.clear();
    extra.clear();
    EXPECT_EQ(pool.available(), k_depth);
}
