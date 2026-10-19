#ifndef VSTREAMER_CORE_DEFAULT_IO_REACTOR_HPP
#define VSTREAMER_CORE_DEFAULT_IO_REACTOR_HPP

#include <bfc/default_reactor.hpp>
#include <functional>

namespace vstreamer
{

using default_io_reactor = bfc::default_reactor<std::function<void()>>;

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_DEFAULT_IO_REACTOR_HPP
