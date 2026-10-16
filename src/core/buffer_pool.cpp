#include "core/buffer_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

namespace vstreamer
{

struct buffer_pool::state
{
    /* One slab for all blocks: a single large allocation (on ESP-IDF it lands in PSRAM instead
     * of thousands of small blocks filling internal RAM). Blocks keep max_align_t alignment. */
    explicit state(size_t block_bytes_in, size_t depth)
        : block_bytes(block_bytes_in)
    {
        constexpr size_t align = alignof(std::max_align_t);
        const size_t     stride = (block_bytes + align - 1) / align * align;
        if (depth > 0 && block_bytes > 0)
        {
            slab = std::make_unique<std::byte[]>(stride * depth);
        }
        free_list.reserve(depth);
        for (size_t i = 0; slab && i < depth; ++i)
        {
            free_list.push_back(slab.get() + i * stride);
        }
    }

    ~state()
    {
        accepting = false;
        std::lock_guard<std::mutex> lock(mu);
        free_list.clear();
    }

    void recycle(std::byte *p)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (accepting)
        {
            free_list.push_back(p);
        }
    }

    std::mutex                              mu;
    std::unique_ptr<std::byte[]>            slab;
    std::vector<std::byte *>                free_list;
    std::atomic<bool>                       accepting {true};
    const size_t                            block_bytes;
    std::atomic<uint64_t>                   misses {0};
};

buffer_pool::buffer_pool(size_t block_bytes, size_t depth) : st(std::make_shared<state>(block_bytes, depth))
{
}

shared_sized_buffer buffer_pool::acquire(size_t need)
{
    if (0 == need)
    {
        return {};
    }

    if (!st || need > st->block_bytes)
    {
        if (st)
        {
            st->misses.fetch_add(1, std::memory_order_relaxed);
        }
        auto buf = shared_sized_buffer::allocate(need);
        if (0 == buf.capacity())
        {
            return {};
        }
        buf.resize(need);
        return buf;
    }

    std::byte *raw = nullptr;
    {
        std::lock_guard<std::mutex> lock(st->mu);
        if (!st->free_list.empty())
        {
            raw = st->free_list.back();
            st->free_list.pop_back();
        }
    }

    if (nullptr == raw)
    {
        st->misses.fetch_add(1, std::memory_order_relaxed);
        auto buf = shared_sized_buffer::allocate(need);
        if (0 == buf.capacity())
        {
            return {};
        }
        buf.resize(need);
        return buf;
    }

    std::shared_ptr<state> keep = st;
    const size_t           cap = st->block_bytes;
    auto release_fn = [keep](std::byte *p) {
        if (keep)
        {
            keep->recycle(p);
        }
    };

    shared_sized_buffer out =
        shared_sized_buffer::adopt(raw, cap, need, std::move(release_fn));
    return out;
}

size_t buffer_pool::available() const
{
    if (!st)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(st->mu);
    return st->free_list.size();
}

uint64_t buffer_pool::misses() const
{
    if (!st)
    {
        return 0;
    }
    return st->misses.load(std::memory_order_relaxed);
}

}  // namespace vstreamer
