#ifndef VSTREAMER_CORE_DEFAULT_REACTOR_HPP
#define VSTREAMER_CORE_DEFAULT_REACTOR_HPP

#include <bfc/default_reactor.hpp>
#include <functional>

namespace vstreamer
{

using default_reactor = bfc::default_reactor<std::function<void()>>;
using default_reactor_registry =
    bfc::object_registry<bfc::object_tag<default_reactor>>;

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_DEFAULT_REACTOR_HPP
