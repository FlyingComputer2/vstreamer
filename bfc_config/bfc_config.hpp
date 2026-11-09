#pragma once
// vstreamer's own bfc config, for its standalone builds (host tests, IDF
// examples). A firmware that builds vstreamer supplies its own config through
// BFC_CONFIG_DIR, and that one wins.
#include <functional>
#include <memory>

#define BFC_CONFIG_ID "vstreamer-default"
#if defined(ESP_PLATFORM)
#define BFC_CONFIG_REACTOR_POLL 1
#else
#define BFC_CONFIG_REACTOR_EPOLL 1
#endif

namespace bfc::config
{
template <typename Signature>
using function = std::function<Signature>;
template <typename T>
using allocator = std::allocator<T>;
} // namespace bfc::config
