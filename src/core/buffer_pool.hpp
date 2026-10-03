#ifndef VSTREAMER_CORE_BUFFER_POOL_HPP
#define VSTREAMER_CORE_BUFFER_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/shared_sized_buffer.hpp"

namespace vstreamer
{

class buffer_pool
{
public:
    buffer_pool(size_t block_bytes, size_t depth);

    /* Pooled block if available and need <= block_bytes, else heap fallback (counted).
     * Returned buffer has size == need. Never returns empty for need > 0 unless OOM. */
    [[nodiscard]] shared_sized_buffer acquire(size_t need);

    [[nodiscard]] size_t available() const;
    [[nodiscard]] uint64_t misses() const;

private:
    struct state;

    std::shared_ptr<state> st;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_BUFFER_POOL_HPP
