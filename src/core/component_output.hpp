#ifndef VSTREAMER_CORE_COMPONENT_OUTPUT_HPP
#define VSTREAMER_CORE_COMPONENT_OUTPUT_HPP

#include "core/component_pdu.hpp"

namespace vstreamer
{

class component_output
{
public:
    virtual ~component_output() = default;

    virtual int output(component_pdu &out) = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_OUTPUT_HPP
