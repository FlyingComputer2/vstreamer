#include "components/v4l2_source.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

TEST(V4l2SourceTest, OpenCloseStressWithoutDevice)
{
    vstreamer::v4l2_source src;
    (void)src.configure("device", "/dev/video255_nonexistent");
    for (int i = 0; i < 200; ++i)
    {
        EXPECT_EQ(0, src.open());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        src.close();
    }
}
