#ifndef VSTREAMER_CORE_FRAME_HPP
#define VSTREAMER_CORE_FRAME_HPP

#include <cstddef>
#include <cstdint>

#include "core/shared_sized_buffer.hpp"

namespace vstreamer
{

enum class media_kind_e : uint8_t
{
    UNKNOWN = 0,
    MJPEG   = 1,
    NV12    = 2,
    H264    = 3,
};

class data_packet;

class frame
{
public:
    frame();
    ~frame();

    frame(const frame &) = delete;
    frame &operator=(const frame &) = delete;

    frame(frame &&other) noexcept;
    frame &operator=(frame &&other) noexcept;

    void release();

    void reset(media_kind_e kind, int width, int height, int64_t pts, bool key,
               shared_sized_buffer payload_in, int64_t capture_mono_ns = 0);

    [[nodiscard]] media_kind_e kind() const { return fields.kind; }
    [[nodiscard]] int          width() const { return fields.width; }
    [[nodiscard]] int          height() const { return fields.height; }
    [[nodiscard]] int64_t      pts() const { return fields.pts; }
    [[nodiscard]] int64_t      capture_mono_ns() const { return fields.capture_mono_ns; }
    [[nodiscard]] bool         key() const { return fields.key; }
    [[nodiscard]] uint8_t     *data() const { return fields.payload.u8(); }
    [[nodiscard]] size_t       size() const { return fields.payload.size(); }
    [[nodiscard]] const shared_sized_buffer &payload_buffer() const { return fields.payload; }

    friend class data_packet;

private:
    struct fields_t
    {
        media_kind_e         kind = media_kind_e::UNKNOWN;
        int                  width = 0;
        int                  height = 0;
        int64_t              pts = 0;
        int64_t              capture_mono_ns = 0;
        bool                 key = false;
        shared_sized_buffer  payload;
    } fields;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_FRAME_HPP
