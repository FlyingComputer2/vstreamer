#include "apps/common/pipeline_controller.hpp"

#include <gtest/gtest.h>


#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

std::mutex               order_mu;
std::vector<std::string> stop_order;
std::atomic<bool>        stage_b_exited {false};

void record_stop(const char *name)
{
    std::lock_guard<std::mutex> lock(order_mu);
    stop_order.push_back(name);
}

}  // namespace

TEST(PipelineControllerTest, StopsStagesInReverseAndRunsMetrics)
{
    stop_order.clear();
    stage_b_exited = false;
    std::atomic<int> metrics_ticks {0};

    vstreamer::apps::pipeline_controller ctrl;
    ctrl.add_metrics_sync([&metrics_ticks] { metrics_ticks.fetch_add(1); });

    ctrl.add_stage("a", "source", [](std::atomic<bool> &run) {
        while (run.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        while (!stage_b_exited.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        record_stop("a");
    });
    ctrl.add_stage("b", "encode", [](std::atomic<bool> &run) {
        while (run.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        record_stop("b");
        stage_b_exited.store(true);
    });

    std::thread runner([&ctrl] { ctrl.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    EXPECT_GE(metrics_ticks.load(), 3);
    const auto t0 = std::chrono::steady_clock::now();
    ctrl.request_stop();
    runner.join();
    const auto dt =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                t0);
    EXPECT_LT(dt.count(), 500);

    ASSERT_EQ(stop_order.size(), 2U);
    EXPECT_EQ("b", stop_order[0]);
    EXPECT_EQ("a", stop_order[1]);
}
