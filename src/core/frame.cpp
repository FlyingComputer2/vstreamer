#include "core/frame.hpp"

#include <utility>

namespace vstreamer
{

frame::frame() = default;

frame::~frame()
{
    release();
}

frame::frame(frame &&other) noexcept : fields(std::move(other.fields))
{
    other.fields = {};
}

frame &frame::operator=(frame &&other) noexcept
{
    if (this != &other)
    {
        fields = std::move(other.fields);
        other.fields = {};
    }
    return *this;
}

void frame::release()
{
    fields.payload.clear();
}

void frame::reset(media_kind_e kind, int width, int height, int64_t pts, bool key,
                  shared_sized_buffer payload_in, int64_t capture_mono_ns)
{
    fields.kind = kind;
    fields.width = width;
    fields.height = height;
    fields.pts = pts;
    fields.capture_mono_ns = capture_mono_ns;
    fields.key = key;
    fields.payload = std::move(payload_in);
}

}  // namespace vstreamer
