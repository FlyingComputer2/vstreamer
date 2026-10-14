#ifndef VSTREAMER_CORE_METRICS_HPP
#define VSTREAMER_CORE_METRICS_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vstreamer
{

struct metric
{
    std::variant<uint64_t, int64_t, double, std::string> value;
    std::mutex mutex;
};

void metric_store(metric &m, uint64_t v);
void metric_store(metric &m, const std::atomic<uint64_t> &counter);
void metric_store(metric &m, int64_t v);
void metric_store(metric &m, double v);
void metric_store(metric &m, const std::string &v);
void metric_store(metric &m, const char *v);

class metrics
{
public:
    metrics();
    ~metrics();

    std::shared_ptr<metric> get_metric(const std::string &name);

    [[nodiscard]] std::string to_string() const;

    /* When filter returns false, the metric is omitted from to_string() output. */
    using metric_name_filter = std::function<bool(std::string_view name)>;
    [[nodiscard]] std::string to_string(metric_name_filter filter) const;

    /* Single metric by full name (e.g. stream_sender.loss_pct). */
    [[nodiscard]] bool format_metric(const std::string &name, std::string *out) const;

private:
    mutable std::mutex mu;

    std::vector<std::pair<std::string, std::shared_ptr<metric>>> entries;
    std::unordered_map<std::string, size_t>                      index;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_METRICS_HPP
