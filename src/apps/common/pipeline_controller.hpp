#ifndef VSTREAMER_APPS_PIPELINE_CONTROLLER_HPP
#define VSTREAMER_APPS_PIPELINE_CONTROLLER_HPP

#include "apps/common/app_console.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace vstreamer::apps
{

class pipeline_controller
{
public:
    using stage_fn = std::function<void(std::atomic<bool> &run)>;

    pipeline_controller();
    ~pipeline_controller();

    pipeline_controller(const pipeline_controller &) = delete;
    pipeline_controller &operator=(const pipeline_controller &) = delete;

    void add_stage(const char *name, const char *cpu_key, stage_fn fn);
    void add_metrics_sync(std::function<void()> sync);
    void set_diag_enabled(bool enabled);

    app_console &console();
    void         request_stop();
    void         run();

    /** Keeps legacy stage globals (e.g. apps::g_run) in sync with the controller run flag. */
    void bind_legacy_run(std::atomic<bool> *legacy_run);

    [[nodiscard]] bool diag_enabled() const
    {
        return diag_enabled_.load(std::memory_order_relaxed);
    }

private:
    struct stage_entry
    {
        std::string name;
        std::string cpu_key;
        stage_fn    fn;
        std::thread thread;
    };

    void install_signal_handlers();
    void metrics_loop();

    std::vector<stage_entry> stages_;
    std::vector<std::function<void()>> metrics_syncs_;
    app_console                        console_;
    std::atomic<bool>                  run_ {true};
    std::atomic<bool>                  diag_enabled_ {false};
    std::atomic<bool>                  stop_requested_ {false};
    std::thread                      metrics_thread_;
    std::atomic<bool>               *legacy_run_ {nullptr};
};

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_PIPELINE_CONTROLLER_HPP
