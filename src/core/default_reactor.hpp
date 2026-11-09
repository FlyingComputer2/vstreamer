#ifndef VSTREAMER_CORE_DEFAULT_REACTOR_HPP
#define VSTREAMER_CORE_DEFAULT_REACTOR_HPP

#include <bfc/default_reactor.hpp>

namespace vstreamer
{

// The project's bfc default reactor (type and callback from its bfc_config.hpp)
// and its one registry, shared with winject-l3 and the firmware.
using default_reactor = bfc::default_reactor<>;
using default_reactor_registry = bfc::default_reactor_registry;

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_DEFAULT_REACTOR_HPP
