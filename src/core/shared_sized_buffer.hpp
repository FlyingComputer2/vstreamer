#ifndef VSTREAMER_CORE_SHARED_SIZED_BUFFER_HPP
#define VSTREAMER_CORE_SHARED_SIZED_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>

namespace vstreamer
{

class shared_sized_buffer
{
public:
    struct storage
    {
        std::byte                       *bytes = nullptr;
        size_t                           capacity = 0;
        std::function<void(std::byte *)> release;

        ~storage();
    };

    shared_sized_buffer() = default;
    shared_sized_buffer(const shared_sized_buffer &) = default;
    shared_sized_buffer &operator=(const shared_sized_buffer &) = default;

    shared_sized_buffer(shared_sized_buffer &&other) noexcept
        : store(std::move(other.store)), off(other.off), len(other.len)
    {
        other.off = 0;
        other.len = 0;
    }

    shared_sized_buffer &operator=(shared_sized_buffer &&other) noexcept
    {
        if (this != &other)
        {
            store = std::move(other.store);
            off = other.off;
            len = other.len;
            other.off = 0;
            other.len = 0;
        }
        return *this;
    }

    static shared_sized_buffer copy_from(const void *data, size_t nbytes);
    static shared_sized_buffer allocate(size_t capacity);
    static shared_sized_buffer adopt(std::byte *p, size_t capacity, size_t size,
                                     std::function<void(std::byte *)> release);

    [[nodiscard]] size_t size() const noexcept { return len; }
    [[nodiscard]] size_t offset() const noexcept { return off; }
    [[nodiscard]] size_t capacity() const noexcept
    {
        if (!store)
        {
            return 0;
        }
        return store->capacity - off;
    }

    [[nodiscard]] bool empty() const noexcept { return !store || len == 0; }

    [[nodiscard]] std::byte *data() const noexcept
    {
        if (!store)
        {
            return nullptr;
        }
        return store->bytes + off;
    }

    [[nodiscard]] uint8_t *u8() const noexcept
    {
        return reinterpret_cast<uint8_t *>(data());
    }

    void clear() noexcept
    {
        store.reset();
        off = 0;
        len = 0;
    }

    void reserve(size_t new_capacity);
    void resize(size_t new_size);

    [[nodiscard]] shared_sized_buffer subview(size_t rel_offset, size_t sub_len) const noexcept;

    [[nodiscard]] long use_count() const noexcept
    {
        return store ? static_cast<long>(store.use_count()) : 0;
    }

private:
    std::shared_ptr<storage> store;
    size_t                   off = 0;
    size_t                   len = 0;
};

inline shared_sized_buffer::storage::~storage()
{
    if (release && bytes != nullptr)
    {
        release(bytes);
        bytes = nullptr;
    }
}

inline shared_sized_buffer shared_sized_buffer::copy_from(const void *data, size_t nbytes)
{
    shared_sized_buffer out;
    if (nullptr == data || 0 == nbytes)
    {
        return out;
    }
    out.resize(nbytes);
    std::memcpy(out.data(), data, nbytes);
    return out;
}

inline shared_sized_buffer shared_sized_buffer::allocate(size_t capacity)
{
    shared_sized_buffer out;
    if (0 == capacity)
    {
        return out;
    }
    auto st = std::make_shared<storage>();
    st->bytes = new std::byte[capacity];
    st->capacity = capacity;
    st->release = [](std::byte *p) { delete[] p; };
    out.store = std::move(st);
    out.off = 0;
    out.len = 0;
    return out;
}

inline shared_sized_buffer shared_sized_buffer::adopt(std::byte *p, size_t cap, size_t size,
                                                      std::function<void(std::byte *)> release_fn)
{
    shared_sized_buffer out;
    if (nullptr == p || 0 == cap || size > cap)
    {
        if (release_fn && p != nullptr)
        {
            release_fn(p);
        }
        return out;
    }
    auto st = std::make_shared<storage>();
    st->bytes = p;
    st->capacity = cap;
    st->release = std::move(release_fn);
    out.store = std::move(st);
    out.off = 0;
    out.len = size;
    return out;
}

inline void shared_sized_buffer::reserve(size_t new_capacity)
{
    if (off != 0)
    {
        return;
    }
    if (new_capacity <= capacity())
    {
        return;
    }
    shared_sized_buffer next = allocate(new_capacity);
    if (!empty())
    {
        std::memcpy(next.data(), data(), len);
        next.len = len;
    }
    *this = std::move(next);
}

inline void shared_sized_buffer::resize(size_t new_size)
{
    if (off != 0)
    {
        return;
    }
    if (new_size > capacity())
    {
        reserve(new_size);
    }
    len = new_size;
}

inline shared_sized_buffer shared_sized_buffer::subview(size_t rel_offset, size_t sub_len) const
    noexcept
{
    shared_sized_buffer out;
    if (!store || rel_offset > len || sub_len > len - rel_offset)
    {
        return out;
    }
    out.store = store;
    out.off = off + rel_offset;
    out.len = sub_len;
    return out;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_SHARED_SIZED_BUFFER_HPP
