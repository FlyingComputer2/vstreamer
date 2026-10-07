#include "core/pdu_wakeup.hpp"
#include "core/time_util.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <thread>

namespace
{

constexpr int64_t k_five_ms_ns = 5'000'000LL;

}  // namespace

TEST(PduWakeupTest, NotifyBeforeWaitIsNotLost)
{
    vstreamer::pdu_wakeup w;
    w.notify();
    w.wait_until(INT64_MAX);
}

TEST(PduWakeupTest, WaitReturnsAtDeadlineWithoutNotify)
{
    vstreamer::pdu_wakeup w;
    const int64_t start = vstreamer::steady_mono_ns();
    const int64_t deadline = start + 25'000'000LL;
    w.wait_until(deadline);
    const int64_t elapsed = vstreamer::steady_mono_ns() - start;
    EXPECT_GE(elapsed, 20'000'000LL - k_five_ms_ns);
    EXPECT_LE(elapsed, 40'000'000LL + k_five_ms_ns);
}

TEST(PduWakeupTest, NotifyFromAnotherThreadWakesWaiter)
{
    vstreamer::pdu_wakeup w;
    std::atomic<bool> done {false};
    std::thread waiter([&]() {
        w.wait_until(INT64_MAX);
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    w.notify();
    waiter.join();
    EXPECT_TRUE(done.load());
}

TEST(PduWakeupTest, ManyNotifiesCoalesce)
{
    vstreamer::pdu_wakeup w;
    for (int i = 0; i < 100; ++i)
    {
        w.notify();
    }
    w.wait_until(INT64_MAX);
    const int64_t start = vstreamer::steady_mono_ns();
    w.wait_until(start + 30'000'000LL);
    const int64_t elapsed = vstreamer::steady_mono_ns() - start;
    EXPECT_GE(elapsed, 25'000'000LL - k_five_ms_ns);
}

TEST(PduWakeupTest, SpuriousWakeToleratedByCallerLoop)
{
    vstreamer::pdu_wakeup w;
    int polls = 0;
    const int64_t deadline = vstreamer::steady_mono_ns() + 50'000'000LL;
    while (polls < 5 && vstreamer::steady_mono_ns() < deadline)
    {
        w.wait_until(vstreamer::steady_mono_ns() + 5'000'000LL);
        ++polls;
    }
    EXPECT_GE(polls, 1);
}
