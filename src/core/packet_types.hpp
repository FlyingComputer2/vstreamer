#ifndef VSTREAMER_CORE_PACKET_TYPES_HPP
#define VSTREAMER_CORE_PACKET_TYPES_HPP

#include <cstdint>

#include "core/frame.hpp"
#include "core/packet_kind.hpp"
#include "core/shared_sized_buffer.hpp"

namespace vstreamer
{

class packet_body
{
public:
    virtual ~packet_body() = default;

    [[nodiscard]] virtual packet_kind_e get_type() const = 0;
};

class frame_data : public packet_body
{
public:
    static constexpr packet_kind_e k_kind = packet_kind_e::FRAME;

    [[nodiscard]] packet_kind_e get_type() const override { return k_kind; }

    media_kind_e         kind = media_kind_e::UNKNOWN;
    int                  width = 0;
    int                  height = 0;
    int64_t              pts = 0;
    int64_t              capture_mono_ns = 0;
    bool                 key = false;
    shared_sized_buffer  buf;
};

class audio_data : public packet_body
{
public:
    static constexpr packet_kind_e k_kind = packet_kind_e::AUDIO;

    [[nodiscard]] packet_kind_e get_type() const override { return k_kind; }

    int64_t             pts = 0;
    int                 sample_rate = 0;
    int                 channels = 0;
    shared_sized_buffer buf;
};

class sock_data : public packet_body
{
public:
    static constexpr packet_kind_e k_kind = packet_kind_e::SOCK;

    [[nodiscard]] packet_kind_e get_type() const override { return k_kind; }

    int64_t             pts = 0;
    /* Producer-defined sequence number (0 if unused). */
    uint16_t            seq = 0;
    shared_sized_buffer buf;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_PACKET_TYPES_HPP
