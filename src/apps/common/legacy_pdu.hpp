#ifndef VSTREAMER_APPS_LEGACY_PDU_HPP
#define VSTREAMER_APPS_LEGACY_PDU_HPP

#include <vector>

#include "core/component_pdu.hpp"
#include "core/data_packet.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"

namespace vstreamer::apps
{

class legacy_to_pdu
{
public:
    explicit legacy_to_pdu(sdu_type_e sock_sdu_type = sdu_type_e::RTP);

    void reset();

    /* Expands one legacy packet into zero or more PDUs (caps + data). */
    void convert(const data_packet &in, std::vector<component_pdu> *out);

private:
    sdu_type_e sock_sdu_type_;
    uint64_t   seq_ = 0;
    bool       have_video_caps_ = false;
    int32_t    last_width_ = 0;
    int32_t    last_height_ = 0;
    sdu_type_e last_frame_type_ = sdu_type_e::UNKNOWN;

    [[nodiscard]] sdu_type_e frame_sdu_type(media_kind_e kind) const;
    void                   maybe_emit_video_caps(sdu_type_e frame_type, int width, int height,
                                                 int64_t capture_mono_ns, uint8_t port,
                                                 std::vector<component_pdu> *out);
};

class pdu_to_legacy
{
public:
    /*
     * 0: out filled with a legacy packet.
     * -EAGAIN: caps consumed, no legacy packet (caller should read next PDU).
     */
    int convert(const component_pdu &in, data_packet *out);

private:
    bool          have_raw_caps_ = false;
    bool          have_coded_caps_ = false;
    video_raw_caps   raw_caps_ {};
    video_coded_caps coded_caps_ {};
};

}  // namespace vstreamer::apps

#endif  // VSTREAMER_APPS_LEGACY_PDU_HPP
