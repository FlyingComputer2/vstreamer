#include "core/pipeline_queue.hpp"

#include <cerrno>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "core/key_util.hpp"
#include "core/sdu_type.hpp"

namespace vstreamer
{
namespace
{

constexpr size_t k_max_capacity = 1024;

int queue_query(const pipeline_queue &q, std::string_view key, std::string *value)
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    if ("depth" == key)
    {
        *value = std::to_string(q.depth());
        return 0;
    }
    if ("capacity" == key)
    {
        *value = std::to_string(q.capacity());
        return 0;
    }
    if ("pushed" == key)
    {
        *value = std::to_string(q.pushed());
        return 0;
    }
    if ("drops" == key)
    {
        *value = std::to_string(q.drops());
        return 0;
    }
    return -ENOTSUP;
}

int queue_configure(pipeline_queue &q, std::string_view key, std::string_view value)
{
    if ("capacity" == key)
    {
        int64_t v = 0;
        if (key_parse_i64(value, &v) != 0 || v < 1 || v > static_cast<int64_t>(k_max_capacity))
        {
            return -EINVAL;
        }
        return q.set_capacity(static_cast<size_t>(v));
    }
    return -ENOTSUP;
}

class pipeline_queue_sink final : public component_sink
{
public:
    explicit pipeline_queue_sink(std::shared_ptr<pipeline_queue> q) : q_(std::move(q)) {}

    [[nodiscard]] std::string name() const override { return "pipeline_queue_sink"; }

    int open() override
    {
        q_->reopen();
        return 0;
    }

    void close() override { q_->close(); }

    int input(component_pdu &&in) override { return q_->push(std::move(in)); }

    int configure(std::string_view key, std::string_view value) override
    {
        return queue_configure(*q_, key, value);
    }

    int query(std::string_view key, std::string *value) const override
    {
        return queue_query(*q_, key, value);
    }

private:
    std::shared_ptr<pipeline_queue> q_;
};

class pipeline_queue_source final : public component_source
{
public:
    explicit pipeline_queue_source(std::shared_ptr<pipeline_queue> q) : q_(std::move(q)) {}

    [[nodiscard]] std::string name() const override { return "pipeline_queue_source"; }

    int  open() override { return 0; }
    void close() override {}

    int output(component_pdu &out) override
    {
        const int r = q_->pop(out);
        return (-ECANCELED == r) ? -EAGAIN : r;
    }

    int configure(std::string_view key, std::string_view value) override
    {
        return queue_configure(*q_, key, value);
    }

    int query(std::string_view key, std::string *value) const override
    {
        return queue_query(*q_, key, value);
    }

private:
    std::shared_ptr<pipeline_queue> q_;
};

}  // namespace

pipeline_queue::pipeline_queue(size_t capacity)
    : capacity_(std::clamp<size_t>(capacity, 1, k_max_capacity))
{
}

/* Drops the oldest data PDUs until at most limit remain. Caps PDUs stay, so the queue may hold
 * more than limit when it is full of caps (never happens in practice). */
void pipeline_queue::trim_locked(size_t limit)
{
    auto it = q_.begin();
    while (q_.size() > limit && it != q_.end())
    {
        if (is_caps(it->sdu_type))
        {
            ++it;
            continue;
        }
        it = q_.erase(it);
        drops_.fetch_add(1, std::memory_order_relaxed);
    }
}

int pipeline_queue::push(component_pdu &&pdu)
{
    std::shared_ptr<pdu_wakeup> wake;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_)
        {
            return -ECANCELED;
        }
        trim_locked(capacity_ - 1);
        q_.push_back(std::move(pdu));
        pushed_.fetch_add(1, std::memory_order_relaxed);
        wake = consumer_wake_;
    }
    if (wake)
    {
        wake->notify();
    }
    return 0;
}

int pipeline_queue::pop(component_pdu &out)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (q_.empty())
    {
        return closed_ ? -ECANCELED : -EAGAIN;
    }
    out = std::move(q_.front());
    q_.pop_front();
    return 0;
}

void pipeline_queue::close()
{
    std::shared_ptr<pdu_wakeup> wake;
    {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        wake = consumer_wake_;
    }
    if (wake)
    {
        wake->notify();
    }
}

/* PDUs left from the previous producer session are dropped: after a restart the consumer must
 * not see old frames or caps for a format that may have changed. */
void pipeline_queue::reopen()
{
    std::lock_guard<std::mutex> lock(mu_);
    q_.clear();
    closed_ = false;
}

void pipeline_queue::bind_consumer_wakeup(std::shared_ptr<pdu_wakeup> w)
{
    std::lock_guard<std::mutex> lock(mu_);
    consumer_wake_ = std::move(w);
}

size_t pipeline_queue::depth() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return q_.size();
}

size_t pipeline_queue::capacity() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return capacity_;
}

uint64_t pipeline_queue::pushed() const
{
    return pushed_.load(std::memory_order_relaxed);
}

uint64_t pipeline_queue::drops() const
{
    return drops_.load(std::memory_order_relaxed);
}

int pipeline_queue::set_capacity(size_t capacity)
{
    if (capacity < 1 || capacity > k_max_capacity)
    {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> lock(mu_);
    capacity_ = capacity;
    trim_locked(capacity_);
    return 0;
}

std::unique_ptr<component_sink> make_pipeline_queue_sink(std::shared_ptr<pipeline_queue> q)
{
    if (!q)
    {
        return nullptr;
    }
    return std::make_unique<pipeline_queue_sink>(std::move(q));
}

std::unique_ptr<component_source> make_pipeline_queue_source(std::shared_ptr<pipeline_queue> q)
{
    if (!q)
    {
        return nullptr;
    }
    return std::make_unique<pipeline_queue_source>(std::move(q));
}

}  // namespace vstreamer
