#ifndef VSTREAMER_CORE_COMPONENT_INPUT_HPP
#define VSTREAMER_CORE_COMPONENT_INPUT_HPP

#include "core/component_pdu.hpp"

namespace vstreamer
{

class component_input
{
public:
    virtual ~component_input() = default;

    virtual int input(component_pdu &&in) = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_COMPONENT_INPUT_HPP
