#ifndef VSTREAMER_CORE_COMPONENT_HPP
#define VSTREAMER_CORE_COMPONENT_HPP

#include <cstdint>

#include <memory>
#include <string>
#include <string_view>

#include <climits>

#include "core/pdu_wakeup.hpp"

namespace vstreamer
{

class component
{
public:
    virtual ~component() = default;

    /* 0 on success, -ENOTSUP unknown key, -EINVAL bad value, other -errno. */
    virtual int configure(std::string_view key, std::string_view value) = 0;
    /* Fills *value (overwritten). 0 / -ENOTSUP / -EINVAL. Thread-safe. */
    virtual int query(std::string_view key, std::string *value) const = 0;

    void set_wakeup(std::shared_ptr<pdu_wakeup> w) { wakeup_ = std::move(w); }

    [[nodiscard]] virtual int64_t next_deadline_ns() const { return INT64_MAX; }

protected:
    void notify_wakeup()
    {
        if (wakeup_)
        {
            wakeup_->notify();
        }
    }

private:
    std::shared_ptr<pdu_wakeup> wakeup_;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_HPP
