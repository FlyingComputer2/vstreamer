#ifndef VSTREAMER_CORE_RTP_H264_HPP
#define VSTREAMER_CORE_RTP_H264_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace vstreamer
{

struct rtp_h264_config
{
    int      mtu = 1400;
    uint8_t  payload_type = 96;
    uint32_t ssrc = 0xC0DE0001u;
    int      fps = 30;
};

/* Annex-B H.264 access unit → complete RTP/UDP datagrams (12-byte RTP header + payload). */
class rtp_h264_packer
{
public:
    explicit rtp_h264_packer(rtp_h264_config cfg);

    void reset();

    /* Clears pending and enqueues new datagrams. pts is a frame index (legacy). */
    int pack_annexb(const uint8_t *data, size_t size, int64_t pts, int64_t capture_rt_ns = 0);

    /* Like pack_annexb but rtp_ts is already in 90 kHz units (from capture ts_us). */
    int pack_annexb_rtp_ts(const uint8_t *data, size_t size, uint32_t rtp_ts,
                           int64_t capture_rt_ns = 0);

    bool pending() const { return !queue.empty(); }

    /* Copies one datagram into dst; returns size or negative errno. */
    int pop_datagram(uint8_t *dst, size_t cap);

private:
    int append_datagram(const uint8_t *payload, int plen, int marker, uint32_t ts,
                        int64_t capture_rt_ns = 0);
    int send_nal(const uint8_t *nal, int len, int marker, uint32_t ts,
                 int64_t capture_rt_ns = 0);
    void cache_param(const uint8_t *nal, int len);

    rtp_h264_config cfg;
    uint16_t        seq = 0;
    uint8_t         sps[256];
    int             sps_len = 0;
    uint8_t         pps[256];
    int             pps_len = 0;
    std::deque<std::vector<uint8_t>> queue;
};

/* One RTP datagram in → depay state; complete AU out as Annex-B. */
class rtp_h264_depacketizer
{
public:
    explicit rtp_h264_depacketizer(int fps_in = 30) : fps(fps_in > 0 ? fps_in : 30) {}

    void reset();

    /* Returns 0 when no AU ready; 1 when au_out filled; negative on error. */
    int feed(const uint8_t *datagram, size_t len, std::vector<uint8_t> *au_out);

    [[nodiscard]] bool au_key() const { return last_au_key; }

    [[nodiscard]] float packet_loss() const { return loss; }

    [[nodiscard]] int64_t au_pts() const { return last_au_frame_pts; }

    [[nodiscard]] int64_t au_capture_rt_ns() const { return last_au_capture_rt_ns; }

    [[nodiscard]] uint64_t need_idr() const { return need_idr_count; }

    [[nodiscard]] uint64_t nal_dropped() const { return nal_dropped_count; }

    [[nodiscard]] uint64_t rtp_reordered() const { return rtp_reordered_count; }

private:
    struct parsed_rtp
    {
        uint16_t seq = 0;
        uint32_t ts = 0;
        bool     marker = false;
        const uint8_t *payload = nullptr;
        size_t           plen = 0;
        int64_t          capture_rt_ns = 0;
    };

    bool parse_rtp(const uint8_t *datagram, size_t len, parsed_rtp *out) const;
    void note_sequence(uint16_t seq);
    void begin_au_if_needed(uint32_t ts);
    void append_annexb_nal(const uint8_t *nal, size_t len);
    void abort_fu();
    int  process_payload(const parsed_rtp &rtp);
    void note_au_timestamps(uint32_t ts, int64_t capture_rt_ns);
    void finish_building_au(uint32_t ts, int64_t capture_rt_ns);
    int  pop_completed_au(std::vector<uint8_t> *au_out);

    struct completed_au_s
    {
        std::vector<uint8_t> bytes;
        uint32_t             ts = 0;
        int64_t              capture_rt_ns = 0;
        bool                 key = false;
        bool                 damaged = false;
    };

    int                 fps = 30;
    int64_t             last_au_frame_pts = 0;
    int64_t             last_au_capture_rt_ns = 0;
    bool                last_au_key = false;

    std::vector<uint8_t> building_au;
    uint32_t             building_ts = 0;
    bool                 have_building_ts = false;
    bool                 building_key = false;
    bool                 building_damaged = false;

    std::vector<uint8_t> fu_buf;
    bool                 fu_active = false;

    uint16_t last_seq = 0;
    bool     have_seq = false;
    uint64_t gap_packets = 0;
    uint64_t received_packets = 0;
    float    loss = 0.f;

    uint64_t need_idr_count = 0;
    uint64_t nal_dropped_count = 0;
    uint64_t rtp_reordered_count = 0;

    int64_t building_capture_rt_ns = 0;
    std::deque<completed_au_s> completed_aus;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_RTP_H264_HPP
