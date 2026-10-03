#ifndef VSTREAMER_APPS_APP_CONSOLE_HPP
#define VSTREAMER_APPS_APP_CONSOLE_HPP

#include <netinet/in.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace vstreamer
{
class metrics;
}

namespace vstreamer::apps
{

constexpr const char k_app_console_loopback_host[] = "127.0.0.1";

/* UDP console: custom handlers first, then built-in help/ping/metrics/get_metric. */
class app_console
{
public:
    using handler_fn = std::function<bool(const char *line, std::string &reply)>;

    app_console();
    ~app_console();

    app_console(const app_console &) = delete;
    app_console &operator=(const app_console &) = delete;

    void set_bind_host(const char *host);
    void set_pipeline_metrics(const vstreamer::metrics *source);
    void set_pipeline_metrics_sync_live(std::function<void()> sync_live);
    void set_source_state_metrics_refresh(std::function<void()> refresh);

    void add_handler(handler_fn handler, std::string help_text);

    /* For unit tests without UDP. */
    bool handle_line(const char *line, std::string &reply);

    int  start(int console_port);
    void stop();

private:
    void console_thread_main();
    void stop_console();
    bool handle_line_impl(const char *line, std::string &reply);
    void send_reply(int reply_fd, const sockaddr_in &reply, const std::string &body);

    struct handler_entry
    {
        handler_fn  fn;
        std::string help;
    };

    std::vector<handler_entry> handlers;
    std::atomic<bool>          console_stop {false};
    int                        console_fd = -1;
    std::thread                console_thread;
    std::string                bind_host = k_app_console_loopback_host;

    const vstreamer::metrics *pipeline_metrics = nullptr;
    std::function<void()>     pipeline_metrics_sync_live;
    std::function<void()>     source_state_metrics_refresh;
};

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_APP_CONSOLE_HPP
