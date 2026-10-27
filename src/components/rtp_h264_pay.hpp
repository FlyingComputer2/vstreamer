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
#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/port_caps.hpp"
#include "core/rtp_h264.hpp"
#include "core/sdu_caps.hpp"

namespace vstreamer
{

class rtp_h264_pay : public component_coder
{
public:
    rtp_h264_pay();
    ~rtp_h264_pay() override;

    rtp_h264_pay(const rtp_h264_pay &) = delete;
    rtp_h264_pay &operator=(const rtp_h264_pay &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;
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

    /*
     * An AU waiting to go out as datagrams. output() writes each datagram's RTP header (and
     * FU-A bytes) into the AU in front of its payload, over bytes that already went out, and
     * hands on a view of the AU: no copy. That needs the AU to be held only here (the PDU
     * contract), so output() first drops the caller's previous datagram; if a consumer still
     * holds one, or there is no room in front (the AU's first datagram), it copies instead.
     */
    struct pending_au
    {
        bfc::shared_sized_buffer    au;
        std::vector<rtp_h264_dgram> dgrams;
        std::vector<uint8_t>        params; /* injected cached SPS/PPS; their dgrams point here */
        size_t                      next = 0;
        uint64_t                    ts_us = 0;
        uint32_t                    rtp_ts = 0;
        int64_t                     capture_rt = 0;
    };

    std::deque<pending_au>      pending;
    size_t                      pending_dgrams = 0;
    std::vector<rtp_h264_dgram> plan_scratch;
    uint64_t                    datagrams_in_place = 0;
    uint64_t                    datagrams_copied = 0;
    static constexpr size_t     k_pending_cap = 2048;
    uint64_t                     datagrams_dropped = 0;
    uint64_t                     out_seq_ = 0;

    buffer_pool pool;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_RTP_H264_PAY_HPP
