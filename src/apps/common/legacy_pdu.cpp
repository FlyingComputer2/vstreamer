#include "apps/common/legacy_pdu.hpp"

#include <cerrno>

#include <memory>

#include "core/data_packet.hpp"
#include "core/sdu_caps.hpp"
#include "core/packet_types.hpp"

namespace vstreamer::apps
{

legacy_to_pdu::legacy_to_pdu(sdu_type_e sock_sdu_type) : sock_sdu_type_(sock_sdu_type) {}

void legacy_to_pdu::reset()
{
    seq_ = 0;
    have_video_caps_ = false;
    last_width_ = 0;
    last_height_ = 0;
    last_frame_type_ = sdu_type_e::UNKNOWN;
}

sdu_type_e legacy_to_pdu::frame_sdu_type(media_kind_e kind) const
{
    switch (kind)
    {
    case media_kind_e::NV12:
        return sdu_type_e::NV12;
    case media_kind_e::MJPEG:
        return sdu_type_e::MJPEG;
    case media_kind_e::H264:
        return sdu_type_e::H264_AU;
    default:
        return sdu_type_e::UNKNOWN;
    }
}

void legacy_to_pdu::maybe_emit_video_caps(sdu_type_e frame_type, int width, int height,
                                        int64_t capture_mono_ns, uint8_t port,
                                        std::vector<component_pdu> *out)
{
    if (nullptr == out)
    {
        return;
    }
    const uint64_t ts_us =
        capture_mono_ns > 0 ? static_cast<uint64_t>(capture_mono_ns / 1000LL) : 0ULL;
    const bool changed =
        !have_video_caps_ || width != last_width_ || height != last_height_ ||
        frame_type != last_frame_type_;
    if (!changed)
    {
        return;
    }
    have_video_caps_ = true;
    last_width_ = width;
    last_height_ = height;
    last_frame_type_ = frame_type;

    if (frame_type == sdu_type_e::NV12)
    {
        video_raw_caps caps {};
        caps.width = width;
        caps.height = height;
        caps.hor_stride = width;
        caps.ver_stride = height;
        out->push_back(make_caps_pdu(sdu_type_e::CAPS_VIDEO_RAW, caps, ts_us, port));
    }
    else
    {
        video_coded_caps caps {};
        caps.width = width;
        caps.height = height;
        out->push_back(make_caps_pdu(sdu_type_e::CAPS_VIDEO_CODED, caps, ts_us, port));
    }
}

void legacy_to_pdu::convert(const data_packet &in, std::vector<component_pdu> *out)
{
    if (nullptr == out || in.empty())
    {
        return;
    }
    const packet_kind_e kind = in.get_type();
    if (kind == packet_kind_e::FRAME)
    {
        const frame_data &f = data_packet::cast<frame_data>(in);
        const sdu_type_e frame_type = frame_sdu_type(f.kind);
        maybe_emit_video_caps(frame_type, f.width, f.height, f.capture_mono_ns, 0, out);

        component_pdu pdu;
        pdu.ts_us = f.capture_mono_ns > 0 ? static_cast<uint64_t>(f.capture_mono_ns / 1000LL) : 0ULL;
        pdu.seq = seq_++;
        pdu.sdu_type = frame_type;
        pdu.port = 0;
        pdu.sdu = f.buf;
        if (f.key)
        {
            pdu.flags |= static_cast<uint8_t>(pdu_flag_e::KEY);
        }
        out->push_back(std::move(pdu));
        return;
    }
    if (kind == packet_kind_e::SOCK)
    {
        const sock_data &s = data_packet::cast<sock_data>(in);
        component_pdu pdu;
        pdu.ts_us = s.pts > 0 ? static_cast<uint64_t>(s.pts) : 0ULL;
        pdu.seq = seq_++;
        pdu.sdu_type = sock_sdu_type_;
        pdu.port = 0;
        pdu.sdu = s.buf;
        out->push_back(std::move(pdu));
    }
}

int pdu_to_legacy::convert(const component_pdu &in, data_packet *out)
{
    if (nullptr == out)
    {
        return -EINVAL;
    }
    if (is_caps(in.sdu_type))
    {
        if (in.sdu_type == sdu_type_e::CAPS_VIDEO_RAW)
        {
            if (read_caps(in, &raw_caps_) != 0)
            {
                return -EINVAL;
            }
            have_raw_caps_ = true;
        }
        else if (in.sdu_type == sdu_type_e::CAPS_VIDEO_CODED)
        {
            if (read_caps(in, &coded_caps_) != 0)
            {
                return -EINVAL;
            }
            have_coded_caps_ = true;
        }
        return -EAGAIN;
    }

    if (in.sdu_type == sdu_type_e::NV12 || in.sdu_type == sdu_type_e::MJPEG ||
        in.sdu_type == sdu_type_e::H264_AU)
    {
        auto body = std::make_shared<frame_data>();
        if (in.sdu_type == sdu_type_e::NV12)
        {
            body->kind = media_kind_e::NV12;
            if (have_raw_caps_)
            {
                body->width = raw_caps_.width;
                body->height = raw_caps_.height;
            }
        }
        else if (in.sdu_type == sdu_type_e::MJPEG)
        {
            body->kind = media_kind_e::MJPEG;
            if (have_coded_caps_)
            {
                body->width = coded_caps_.width;
                body->height = coded_caps_.height;
            }
        }
        else
        {
            body->kind = media_kind_e::H264;
            if (have_coded_caps_)
            {
                body->width = coded_caps_.width;
                body->height = coded_caps_.height;
            }
        }
        body->capture_mono_ns = static_cast<int64_t>(in.ts_us) * 1000LL;
        body->pts = body->capture_mono_ns;
        body->key = has_flag(in, pdu_flag_e::KEY);
        body->buf = in.sdu;
        *out = data_packet(std::move(body));
        return 0;
    }

    if (in.sdu_type == sdu_type_e::RTP || in.sdu_type == sdu_type_e::STREAM_DGRAM)
    {
        auto body = std::make_shared<sock_data>();
        body->pts = static_cast<int64_t>(in.ts_us);
        body->seq = static_cast<uint16_t>(in.seq & 0xFFFFU);
        body->buf = in.sdu;
        *out = data_packet(std::move(body));
        return 0;
    }

    return -EINVAL;
}

}  // namespace vstreamer::apps
