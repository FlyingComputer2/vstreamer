#include "core/shared_sized_buffer.hpp"

#include <cstring>
#include <gtest/gtest.h>

using vstreamer::shared_sized_buffer;

TEST(SharedSizedBufferTest, CopyFromAndSubviewShareStorage)
{
    const uint8_t raw[] = {0, 1, 2, 3, 4, 5, 6, 7};
    shared_sized_buffer whole = shared_sized_buffer::copy_from(raw, sizeof(raw));
    EXPECT_EQ(whole.size(), sizeof(raw));
    EXPECT_FALSE(whole.empty());

    shared_sized_buffer slice = whole.subview(2, 3);
    EXPECT_EQ(slice.size(), 3u);
    EXPECT_EQ(slice.u8()[0], 2);
    EXPECT_EQ(slice.u8()[2], 4);

    EXPECT_EQ(whole.use_count(), slice.use_count());

    shared_sized_buffer moved = std::move(slice);
    EXPECT_TRUE(slice.empty());
    EXPECT_EQ(moved.size(), 3u);
}

TEST(SharedSizedBufferTest, ReserveResizeForRecv)
{
    shared_sized_buffer buf;
    buf.reserve(64);
    EXPECT_GE(buf.capacity(), 64u);
    buf.resize(8);
    EXPECT_EQ(buf.size(), 8u);
    std::memcpy(buf.data(), "abcdefgh", 8);

    shared_sized_buffer moved = std::move(buf);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(moved.size(), 8u);

    moved.clear();
    moved.reserve(32);
    moved.resize(4);
    EXPECT_EQ(moved.capacity(), 32u);
}

TEST(SharedSizedBufferTest, InvalidSubviewIsEmpty)
{
    const uint8_t raw[] = {1, 2, 3};
    shared_sized_buffer whole = shared_sized_buffer::copy_from(raw, sizeof(raw));
    EXPECT_TRUE(whole.subview(4, 1).empty());
    EXPECT_TRUE(whole.subview(1, 3).empty());
}

TEST(SharedSizedBufferTest, SubviewKeepsStorageAlive)
{
    const uint8_t raw[] = {9, 8, 7};
    shared_sized_buffer whole = shared_sized_buffer::copy_from(raw, sizeof(raw));
    shared_sized_buffer slice = whole.subview(1, 1);
    const long ref_while_whole = whole.use_count();
    whole.clear();
    EXPECT_FALSE(slice.empty());
    EXPECT_EQ(slice.u8()[0], 8);
    EXPECT_GE(slice.use_count(), 1);
    EXPECT_GE(ref_while_whole, 2);
}
