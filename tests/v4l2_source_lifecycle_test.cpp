#include "components/v4l2_source.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace
{

uint64_t read_thread_utime_jiffies(pid_t tid)
{
    std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/stat");
    if (!in)
    {
        return 0;
    }
    std::string line;
    std::getline(in, line);
    std::istringstream iss(line);
    std::string        token;
    for (int field = 1; field <= 13 && iss >> token; ++field)
    {
        if (field == 13)
        {
            return static_cast<uint64_t>(std::stoull(token));
        }
    }
    return 0;
}

std::vector<pid_t> list_thread_ids()
{
    std::vector<pid_t> out;
    std::ifstream      tasks("/proc/self/task");
    std::string        tid;
    while (std::getline(tasks, tid))
    {
        if (!tid.empty())
        {
            out.push_back(static_cast<pid_t>(std::stoi(tid)));
        }
    }
    return out;
}

uint64_t sum_thread_utime_jiffies()
{
    uint64_t sum = 0;
    for (const pid_t tid : list_thread_ids())
    {
        sum += read_thread_utime_jiffies(tid);
    }
    return sum;
}

}  // namespace

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

TEST(V4l2SourceTest, PollWatcherIdleCpuWhileCaptureUnavailable)
{
    vstreamer::v4l2_source src;
    (void)src.configure("device", "/dev/video255_nonexistent");
    ASSERT_EQ(0, src.open());
    const uint64_t t0 = sum_thread_utime_jiffies();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t t1 = sum_thread_utime_jiffies();
    src.close();
    const uint64_t delta = t1 - t0;
    EXPECT_LT(delta, 50U) << "poll watcher should sleep when capture fd is unavailable (jiffies)";
}
