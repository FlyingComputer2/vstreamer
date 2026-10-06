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
#include "core/rtp_h264.hpp"

namespace vstreamer
{

class rtp_h264_depay : public component_coder
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

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    struct au_item
    {
        std::vector<uint8_t> buf;
        int64_t              pts = 0;
        int64_t              capture_mono_ns = 0;
        bool                 key = false;
    };

    void push_au(au_item &&item);

    mutable std::mutex mu;
    bool               opened = false;
    int                fps = 30;

    rtp_h264_depacketizer depay {30};
    std::deque<au_item>   au_queue;
    static constexpr size_t k_au_queue_depth = 8;
    uint64_t              au_dropped = 0;
    uint64_t              capture_ts_rejected = 0;
    double                capture_skew_ms = 0.0;
    double                last_node_latency_ms = 0.0;

    [[nodiscard]] int64_t accept_capture_rt_ns(int64_t capture_rt_ns);
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_RTP_H264_DEPAY_HPP
