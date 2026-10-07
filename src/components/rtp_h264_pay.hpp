#ifndef VSTREAMER_COMPONENTS_RTP_H264_PAY_HPP
#define VSTREAMER_COMPONENTS_RTP_H264_PAY_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_RTP_H264_PAY
#error "rtp_h264_pay requires -DENABLE_RTP_H264_PAY=ON"
#endif

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/buffer_pool.hpp"
#include "core/component.hpp"
#include "core/component_coder.hpp"
#include "core/data_packet.hpp"
#include "core/pdu_input.hpp"
#include "core/pdu_output.hpp"
#include "core/port_caps.hpp"
#include "core/rtp_h264.hpp"
#include "core/sdu_caps.hpp"

namespace vstreamer
{

class rtp_h264_pay : public component_coder, public pdu_input, public pdu_output
{
public:
    rtp_h264_pay();
    ~rtp_h264_pay() override;

    rtp_h264_pay(const rtp_h264_pay &) = delete;
    rtp_h264_pay &operator=(const rtp_h264_pay &) = delete;

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
    void rebuild_packer();
    void recreate_pool_locked();
    [[nodiscard]] int input_pdu_locked(component_pdu &&in);

    static const std::vector<port_desc> &input_ports();
    static const std::vector<port_desc> &output_ports();

    mutable std::mutex mu;
    bool               opened = false;

    int         mtu = 1400;
    int         fps = 30;
    uint8_t     pt = 96;
    uint32_t    ssrc = 0xC0DE0001u;
    rtp_h264_packer packer {rtp_h264_config {}};

    bool              have_coded_caps_ = false;
    video_coded_caps  coded_caps_ {};

    struct pending_datagram
    {
        std::vector<uint8_t> bytes;
        uint64_t             ts_us = 0;
        bool                 au_end = false;
    };

    std::deque<pending_datagram> pending;
    static constexpr size_t      k_pending_cap = 512;
    uint64_t                     datagrams_dropped = 0;
    uint64_t                     out_seq_ = 0;

    buffer_pool pool;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_RTP_H264_PAY_HPP
