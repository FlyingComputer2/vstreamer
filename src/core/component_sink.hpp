#ifndef VSTREAMER_CORE_COMPONENT_SINK_HPP
#define VSTREAMER_CORE_COMPONENT_SINK_HPP

#include <string>

#include "core/component.hpp"
#include "core/component_input.hpp"

namespace vstreamer
{

class component_sink : public component, public component_input
{
public:
    ~component_sink() override = default;

    [[nodiscard]] virtual std::string name() const = 0;

    virtual int  open() = 0;
    virtual void close() = 0;

    virtual int set_enabled(bool on, int timeout_ms);
    [[nodiscard]] virtual bool enabled() const;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_SINK_HPP
