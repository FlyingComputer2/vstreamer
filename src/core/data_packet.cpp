#include "core/data_packet.hpp"

#include <memory>
#include <utility>

namespace vstreamer
{

void data_packet::adopt_frame(frame &&fr)
{
    auto fd = std::make_shared<frame_data>();
    fd->kind = fr.kind();
    fd->width = fr.width();
    fd->height = fr.height();
    fd->pts = fr.pts();
    fd->capture_mono_ns = fr.capture_mono_ns();
    fd->key = fr.key();
    fd->buf = std::move(fr.fields.payload);
    body = std::move(fd);
}

void data_packet::move_to_frame(frame &out)
{
    frame_data &f = cast<frame_data>(*this);
    out.reset(f.kind, f.width, f.height, f.pts, f.key, std::move(f.buf), f.capture_mono_ns);
    release();
}

}  // namespace vstreamer
