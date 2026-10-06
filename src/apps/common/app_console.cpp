#include "apps/common/app_console.hpp"

#include "core/metrics.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

namespace vstreamer::apps
{
namespace
{

void trim_inplace(char *s)
{
    if (nullptr == s)
    {
        return;
    }
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r')
    {
        start++;
    }
    if (start != s)
    {
        std::memmove(s, start, std::strlen(start) + 1);
    }
    size_t len = std::strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r'))
    {
        s[len - 1] = '\0';
        len--;
    }
}

int bind_ipv4_host(sockaddr_in *in, const std::string &host, int port)
{
    if (nullptr == in)
    {
        return -EINVAL;
    }
    std::memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_port = htons(static_cast<uint16_t>(port));
    if (host == "0.0.0.0" || host == "*")
    {
        in->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }
    if (inet_pton(AF_INET, host.c_str(), &in->sin_addr) != 1)
    {
        return -EINVAL;
    }
    return 0;
}

}  // namespace

app_console::app_console() = default;

app_console::~app_console()
{
    stop();
}

void app_console::set_bind_host(const char *host)
{
    if (nullptr == host || host[0] == '\0')
    {
        bind_host = k_app_console_loopback_host;
        return;
    }
    bind_host = host;
}

void app_console::set_pipeline_metrics(const vstreamer::metrics *source)
{
    pipeline_metrics = source;
}

void app_console::set_pipeline_metrics_sync_live(std::function<void()> sync_live)
{
    pipeline_metrics_sync_live = std::move(sync_live);
}

void app_console::set_source_state_metrics_refresh(std::function<void()> refresh)
{
    source_state_metrics_refresh = std::move(refresh);
}

void app_console::set_metric_name_filter(metric_name_filter_fn filter)
{
    metric_filter = std::move(filter);
}

void app_console::add_handler(handler_fn handler, std::string help_text)
{
    handlers.push_back({std::move(handler), std::move(help_text)});
}

bool app_console::handle_line(const char *line, std::string &reply)
{
    return handle_line_impl(line, reply);
}

bool app_console::handle_line_impl(const char *line, std::string &reply)
{
    char work[512];
    std::snprintf(work, sizeof(work), "%s", line);
    trim_inplace(work);

    for (const handler_entry &h : handlers)
    {
        if (h.fn && h.fn(work, reply))
        {
            return true;
        }
    }

    if (0 == std::strcmp(work, "help") || 0 == std::strcmp(work, "h") ||
        0 == std::strcmp(work, "?"))
    {
        reply.clear();
        for (const handler_entry &h : handlers)
        {
            if (!h.help.empty())
            {
                reply += h.help;
                if (reply.empty() || reply.back() != '\n')
                {
                    reply.push_back('\n');
                }
            }
        }
        reply +=
            "get_metric <metric_name>\n"
            "ping\n"
            "metrics\n"
            "get\n"
            "(empty line)  pipeline metrics\n";
        return true;
    }

    if (0 == std::strcmp(work, "ping"))
    {
        reply = "pong\n";
        return true;
    }

    if (0 == std::strncmp(work, "get_metric ", 11))
    {
        char *name = work + 11;
        trim_inplace(name);
        if ('\0' == name[0])
        {
            reply = "err metric name required\n";
            return true;
        }
        if (nullptr == pipeline_metrics)
        {
            reply = "err metrics not configured\n";
            return true;
        }
        if (0 == std::strcmp(name, "source.state") && source_state_metrics_refresh)
        {
            source_state_metrics_refresh();
        }
        if (metric_filter && !metric_filter(name))
        {
            reply = "err unknown metric\n";
            return true;
        }
        std::string value;
        if (!pipeline_metrics->format_metric(name, &value))
        {
            reply = "err unknown metric\n";
            return true;
        }
        value.push_back('\n');
        reply = std::move(value);
        return true;
    }

    if (0 == std::strcmp(work, "metrics") || 0 == std::strcmp(work, "get") || '\0' == work[0])
    {
        if (nullptr == pipeline_metrics)
        {
            reply = "err metrics not configured\n";
            return true;
        }
        if (pipeline_metrics_sync_live)
        {
            pipeline_metrics_sync_live();
        }
        else if (source_state_metrics_refresh)
        {
            source_state_metrics_refresh();
        }
        const std::string report = metric_filter
                                       ? pipeline_metrics->to_string(
                                             [this](std::string_view name) {
                                                 return metric_filter(name);
                                             })
                                       : pipeline_metrics->to_string();
        if (report.empty())
        {
            reply = "err metrics empty\n";
            return true;
        }
        reply = report;
        return true;
    }

    reply = "err: unknown command\n";
    return true;
}

void app_console::send_reply(int reply_fd, const sockaddr_in &reply, const std::string &body)
{
    if (reply_fd < 0 || body.empty())
    {
        return;
    }
    sendto(reply_fd, body.data(), body.size(), 0, reinterpret_cast<const sockaddr *>(&reply),
           sizeof(reply));
}

void app_console::console_thread_main()
{
    uint8_t buf[512];
    while (!console_stop.load())
    {
        if (console_fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        sockaddr_in from {};
        socklen_t   from_len = sizeof(from);
        const ssize_t n =
            recvfrom(console_fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from),
                     &from_len);
        if (n <= 0)
        {
            if (console_stop.load())
            {
                break;
            }
            continue;
        }
        buf[n] = '\0';

        char *nl = static_cast<char *>(std::memchr(buf, '\n', static_cast<size_t>(n)));
        if (nullptr != nl)
        {
            *nl = '\0';
        }
        std::string reply;
        handle_line_impl(reinterpret_cast<char *>(buf), reply);
        send_reply(console_fd, from, reply);
    }
}

int app_console::start(int console_port)
{
    stop_console();

    console_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (console_fd < 0)
    {
        return -errno;
    }

    sockaddr_in in {};
    const int   bind_r = bind_ipv4_host(&in, bind_host, console_port);
    if (bind_r < 0)
    {
        ::close(console_fd);
        console_fd = -1;
        return bind_r;
    }
    if (bind(console_fd, reinterpret_cast<sockaddr *>(&in), sizeof(in)) < 0)
    {
        const int err = -errno;
        ::close(console_fd);
        console_fd = -1;
        return err;
    }

    console_stop = false;
    console_thread = std::thread(&app_console::console_thread_main, this);
    return 0;
}

void app_console::stop_console()
{
    console_stop = true;
    if (console_fd >= 0)
    {
        ::shutdown(console_fd, SHUT_RDWR);
    }
    if (console_thread.joinable())
    {
        console_thread.join();
    }
    if (console_fd >= 0)
    {
        ::close(console_fd);
        console_fd = -1;
    }
    console_stop = false;
}

void app_console::stop()
{
    stop_console();
}

}  // namespace vstreamer::apps
