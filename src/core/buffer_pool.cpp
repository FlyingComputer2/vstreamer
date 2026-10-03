#include "core/buffer_pool.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace vstreamer
{

struct buffer_pool::state
{
    explicit state(size_t block_bytes_in, size_t depth)
        : block_bytes(block_bytes_in)
    {
        blocks.reserve(depth);
        free_list.reserve(depth);
        for (size_t i = 0; i < depth; ++i)
        {
            auto mem = std::make_unique<std::byte[]>(block_bytes);
            free_list.push_back(mem.get());
            blocks.push_back(std::move(mem));
        }
    }

    ~state()
    {
        accepting = false;
        std::lock_guard<std::mutex> lock(mu);
        free_list.clear();
        blocks.clear();
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
    std::vector<std::unique_ptr<std::byte[]>> blocks;
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
