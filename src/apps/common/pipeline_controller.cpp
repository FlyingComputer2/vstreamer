#include "apps/common/pipeline_controller.hpp"

#include "apps/common/stage_latency.hpp"

#include <csignal>
#include <cstdio>
#include <thread>

#include <chrono>

namespace vstreamer::apps
{
namespace
{

pipeline_controller *g_active_controller = nullptr;

void on_signal(int /*sig*/)
{
    if (nullptr != g_active_controller)
    {
        g_active_controller->request_stop();
    }
}

}  // namespace

pipeline_controller::pipeline_controller() = default;

pipeline_controller::~pipeline_controller()
{
    request_stop();
    if (metrics_thread_.joinable())
    {
        metrics_thread_.join();
    }
    for (auto it = stages_.rbegin(); it != stages_.rend(); ++it)
    {
        if (it->thread.joinable())
        {
            it->thread.join();
        }
    }
    if (g_active_controller == this)
    {
        g_active_controller = nullptr;
    }
}

void pipeline_controller::add_stage(const char *name, const char *cpu_key, stage_fn fn)
{
    stage_entry e;
    e.name = (nullptr != name) ? name : "";
    e.cpu_key = (nullptr != cpu_key) ? cpu_key : "";
    e.fn = std::move(fn);
    stages_.push_back(std::move(e));
}

void pipeline_controller::add_metrics_sync(std::function<void()> sync)
{
    metrics_syncs_.push_back(std::move(sync));
}

void pipeline_controller::set_diag_enabled(bool enabled)
{
    diag_enabled_.store(enabled, std::memory_order_relaxed);
    stage_latency_set_diag_enabled(enabled);
}

app_console &pipeline_controller::console()
{
    return console_;
}

void pipeline_controller::request_stop()
{
    stop_requested_.store(true, std::memory_order_release);
    run_.store(false, std::memory_order_release);
}

void pipeline_controller::install_signal_handlers()
{
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void pipeline_controller::metrics_loop()
{
    while (run_.load(std::memory_order_acquire))
    {
        for (const auto &sync : metrics_syncs_)
        {
            if (sync)
            {
                sync();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void pipeline_controller::run()
{
    run_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    g_active_controller = this;
    install_signal_handlers();

    for (stage_entry &s : stages_)
    {
        s.thread = std::thread([&, fn = s.fn] { fn(run_); });
    }

    metrics_thread_ = std::thread([this] { metrics_loop(); });

    while (!stop_requested_.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    run_.store(false, std::memory_order_release);

    if (metrics_thread_.joinable())
    {
        metrics_thread_.join();
    }

    for (auto it = stages_.rbegin(); it != stages_.rend(); ++it)
    {
        if (it->thread.joinable())
        {
            it->thread.join();
        }
    }

    g_active_controller = nullptr;
}

}  // namespace vstreamer::apps
