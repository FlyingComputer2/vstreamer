#ifndef VSTREAMER_CORE_THREAD_SPAWN_HPP
#define VSTREAMER_CORE_THREAD_SPAWN_HPP

#include <cstddef>
#include <thread>
#include <utility>

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
    /* ESP-IDF stack size; 0 keeps the pthread default (CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT,
     * ~3 KB). Linux threads keep the system default. */
    size_t stack_bytes = 0;
    /* ESP-IDF core, fixed at creation (FreeRTOS cannot move a task later); -1 = any core.
     * Ignored on Linux, where pin_current_thread_to_cpu() pins a running thread. */
    int cpu = -1;
    /* FreeRTOS priority (0..configMAX_PRIORITIES-1); -1 keeps the pthread default. */
    int prio = -1;
};

/*
 * std::thread with a stack size, name and core on ESP-IDF; a plain std::thread on Linux.
 * The new thread does not inherit these settings: threads it starts get the defaults.
 */
template <typename... Args>
[[nodiscard]] std::thread spawn_thread(const thread_opts &opts, Args &&...args)
{
#if defined(ESP_PLATFORM)
    /* esp_pthread_set_cfg() applies to threads the calling thread creates; restore it after. */
    esp_pthread_cfg_t prev {};
    const bool        had_prev = esp_pthread_get_cfg(&prev) == ESP_OK;

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    if (opts.stack_bytes > 0)
    {
        cfg.stack_size = opts.stack_bytes;
    }
    cfg.pin_to_core = (opts.cpu >= 0 && opts.cpu < portNUM_PROCESSORS) ? opts.cpu : tskNO_AFFINITY;
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
    (void)opts;
    return std::thread(std::forward<Args>(args)...);
#endif
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_THREAD_SPAWN_HPP
