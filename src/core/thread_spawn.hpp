#ifndef VSTREAMER_CORE_THREAD_SPAWN_HPP
#define VSTREAMER_CORE_THREAD_SPAWN_HPP

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "core/key_util.hpp"
#include "core/thread_affinity.hpp"

#if defined(ESP_PLATFORM)
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace vstreamer
{

struct thread_opts
{
    /* ESP-IDF task name (up to configMAX_TASK_NAME_LEN - 1 chars). */
    const char *name = nullptr;
    /* ESP-IDF stack size; 0 keeps the pthread default
     * (CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT, ~3 KB). Linux threads keep the
     * system default. */
    size_t stack_bytes = 0;
    /* Core the thread runs on; -1 = any core. ESP-IDF fixes it at creation
     * (FreeRTOS cannot move a task later). Linux pins the new thread before it
     * runs the callable, so threads it starts inherit the pin. */
    int cpu = -1;
    /* FreeRTOS priority (0..configMAX_PRIORITIES-1); -1 keeps the pthread
     * default. Ignored on Linux (real-time priorities need privileges). */
    int prio = -1;
};

/*
 * std::thread with a stack size, name, core and priority on ESP-IDF; on Linux
 * only the core. On ESP-IDF the new thread does not inherit these settings:
 * threads it starts get the defaults.
 */
template <typename... Args>
[[nodiscard]] std::thread spawn_thread(const thread_opts &opts, Args &&...args)
{
#if defined(ESP_PLATFORM)
    /* esp_pthread_set_cfg() applies to threads the calling thread creates;
     * restore it after. */
    esp_pthread_cfg_t prev{};
    const bool had_prev = esp_pthread_get_cfg(&prev) == ESP_OK;

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    if (opts.stack_bytes > 0)
    {
        cfg.stack_size = opts.stack_bytes;
    }
    cfg.pin_to_core = (opts.cpu >= 0 && opts.cpu < portNUM_PROCESSORS)
                          ? opts.cpu
                          : tskNO_AFFINITY;
    if (opts.prio >= 0)
    {
        cfg.prio = static_cast<size_t>(opts.prio);
    }
    cfg.thread_name = opts.name;
    cfg.inherit_cfg = false;
    (void)esp_pthread_set_cfg(&cfg);

    std::thread t(std::forward<Args>(args)...);

    if (had_prev)
    {
        (void)esp_pthread_set_cfg(&prev);
    }
    else
    {
        const esp_pthread_cfg_t def = esp_pthread_get_default_config();
        (void)esp_pthread_set_cfg(&def);
    }
    return t;
#else
    if (opts.cpu < 0)
    {
        return std::thread(std::forward<Args>(args)...);
    }
    return std::thread(
        [cpu = opts.cpu](auto &&...a)
        {
            pin_current_thread_to_cpu(cpu);
            std::invoke(std::forward<decltype(a)>(a)...);
        },
        std::forward<Args>(args)...);
#endif
}

/*
 * Placement of a thread a component starts, so the app (not the component)
 * picks the core and priority. The component exposes it as configure()/query()
 * keys "<role>_cpu" and
 * "<role>_prio" (-1 = any core / default priority, the default) and reads it
 * when it starts the thread, so a change applies on the next open().
 */
struct thread_place
{
    std::atomic<int> cpu{-1};
    std::atomic<int> prio{-1};

    [[nodiscard]] thread_opts opts(const char *name, size_t stack_bytes) const
    {
        return thread_opts{name, stack_bytes,
                           cpu.load(std::memory_order_relaxed),
                           prio.load(std::memory_order_relaxed)};
    }
};

namespace detail
{

/* "<role>_cpu" -> &place->cpu, "<role>_prio" -> &place->prio, else nullptr. */
template <typename Place>
auto thread_place_field(Place *place, std::string_view role,
                        std::string_view key,
                        bool *is_cpu) -> decltype(&place->cpu)
{
    if (key.size() <= role.size() || key.substr(0, role.size()) != role ||
        key[role.size()] != '_')
    {
        return nullptr;
    }
    const std::string_view field = key.substr(role.size() + 1);
    if ("cpu" == field)
    {
        *is_cpu = true;
        return &place->cpu;
    }
    if ("prio" == field)
    {
        *is_cpu = false;
        return &place->prio;
    }
    return nullptr;
}

}  // namespace detail

/* Handles "<role>_cpu" / "<role>_prio": 0, -EINVAL (out of range), or -ENOTSUP
 * (other key). */
inline int thread_place_configure(thread_place *place, std::string_view role,
                                  std::string_view key, std::string_view value)
{
    bool is_cpu = false;
    auto *field = detail::thread_place_field(place, role, key, &is_cpu);
    if (nullptr == field)
    {
        return -ENOTSUP;
    }
#if defined(ESP_PLATFORM)
    const int64_t max =
        is_cpu ? portNUM_PROCESSORS - 1 : configMAX_PRIORITIES - 1;
#else
    const int64_t max = is_cpu ? 1023 : 99;
#endif
    int64_t v = 0;
    if (key_parse_i64(value, &v) < 0 || v < -1 || v > max)
    {
        return -EINVAL;
    }
    field->store(static_cast<int>(v), std::memory_order_relaxed);
    return 0;
}

/* Answers "<role>_cpu" / "<role>_prio"; -ENOTSUP for other keys. */
inline int thread_place_query(const thread_place &place, std::string_view role,
                              std::string_view key, std::string *value)
{
    bool is_cpu = false;
    const auto *field = detail::thread_place_field(&place, role, key, &is_cpu);
    if (nullptr == field)
    {
        return -ENOTSUP;
    }
    *value = std::to_string(field->load(std::memory_order_relaxed));
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_THREAD_SPAWN_HPP
