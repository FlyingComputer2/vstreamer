#ifndef VSTREAMER_CORE_DEFAULT_REACTOR_HOST_HPP
#define VSTREAMER_CORE_DEFAULT_REACTOR_HOST_HPP

#include "core/default_reactor.hpp"
#include "core/thread_spawn.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace vstreamer
{

/* Application-owned reactor. Declare before its senders so they close before it stops. */
class default_reactor_host
{
public:
    default_reactor_host()
    {
        static std::atomic<unsigned long> next {0};
        do
        {
            name_ = "vstreamer-host-" + std::to_string(next.fetch_add(1));
        } while (!default_reactor_registry::get_instance().add(name_, reactor_));
        auto started = std::make_shared<std::promise<void>>();
        auto ready = started->get_future();
        reactor_->wake_up([started] { started->set_value(); });
        worker_ = spawn_thread(thread_opts {"vs_reactor", 4 * 1024},
                               [this] { reactor_->run(); });
        ready.wait();
    }

    ~default_reactor_host()
    {
        reactor_->stop();
        worker_.join();
        default_reactor_registry::get_instance().rem(name_);
    }

    default_reactor_host(const default_reactor_host &) = delete;
    default_reactor_host &operator=(const default_reactor_host &) = delete;

    const std::string &name() const { return name_; }

private:
    std::shared_ptr<default_reactor> reactor_ = std::make_shared<default_reactor>();
    std::string name_;
    std::thread worker_;
};

} // namespace vstreamer

#endif // VSTREAMER_CORE_DEFAULT_REACTOR_HOST_HPP
