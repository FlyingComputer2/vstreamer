#ifndef VSTREAMER_COMPONENTS_RTP_H264_DEPAY_HPP
#define VSTREAMER_COMPONENTS_RTP_H264_DEPAY_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_RTP_H264_DEPAY
#error "rtp_h264_depay requires -DENABLE_RTP_H264_DEPAY=ON"
#endif

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/component_coder.hpp"
#include "core/data_packet.hpp"
#include "core/pdu_input.hpp"
#include "core/pdu_output.hpp"
#include "core/port_caps.hpp"
#include "core/rtp_h264.hpp"
#include "core/sdu_caps.hpp"

namespace vstreamer
{

class rtp_h264_depay : public component_coder, public pdu_input, public pdu_output
{
public:
    rtp_h264_depay();
    ~rtp_h264_depay() override;

    rtp_h264_depay(const rtp_h264_depay &) = delete;
    rtp_h264_depay &operator=(const rtp_h264_depay &) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e input_kind() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    [[nodiscard]] packet_kind_e input_packet_kind() const override;
    [[nodiscard]] packet_kind_e output_packet_kind() const override;

    int  open() override;
    void close() override;

    int input(uint8_t port, const data_packet &in) override;
    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int input(component_pdu &&in) override;
    int output(component_pdu &out) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    struct au_item
    {
        std::vector<uint8_t> buf;
        uint64_t             ts_us = 0;
        bool                 key = false;
    };

    void push_au(au_item &&item);
    void maybe_queue_caps_for_au(const au_item &item);
    [[nodiscard]] int64_t accept_capture_rt_ns(int64_t capture_rt_ns);

    static const std::vector<port_desc> &input_ports();
    static const std::vector<port_desc> &output_ports();

    mutable std::mutex mu;
    bool               opened = false;
    int                fps = 30;

    rtp_h264_depacketizer depay {30};
    std::deque<component_pdu> out_queue;
    static constexpr size_t   k_au_queue_depth = 8;
    uint64_t                  au_dropped = 0;
    uint64_t                  capture_ts_rejected = 0;
    double                    capture_skew_ms = 0.0;

    int32_t last_caps_w_ = 0;
    int32_t last_caps_h_ = 0;
    uint64_t out_seq_ = 0;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_RTP_H264_DEPAY_HPP
