#ifndef VSTREAMER_CORE_MKV_WRITER_HPP
#define VSTREAMER_CORE_MKV_WRITER_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace vstreamer
{

enum class mkv_codec_e : uint8_t
{
    MJPEG,
    H264,
};

struct mkv_track_config
{
    mkv_codec_e codec = mkv_codec_e::MJPEG;
    int32_t     width = 0;
    int32_t     height = 0;
    int32_t     fps = 30;
};

/*
 * Single-video-track Matroska writer on stdio; no libav. Works wherever fopen() does
 * (Linux, ESP-IDF FATFS on /sdcard).
 *
 * Crash safety: the Segment is written with unknown size and each Cluster is flushed
 * and fsync'ed when it closes, so a file cut off by power loss plays up to the last
 * finished cluster (~1 s). close() patches sizes, Duration, Cues and the SeekHead.
 *
 * H.264 input is Annex-B access units. The file header is deferred until the first
 * keyframe with SPS and PPS seen (avcC goes into CodecPrivate); earlier frames are
 * skipped. NALs are stored with 4-byte length prefixes; AUDs are dropped.
 */
class mkv_writer
{
public:
    mkv_writer() = default;
    ~mkv_writer();

    mkv_writer(const mkv_writer &) = delete;
    mkv_writer &operator=(const mkv_writer &) = delete;

    /* Sets the output; the file is created on the first written frame. Returns 0 or
     * negative errno. */
    int open(const std::string &path, const mkv_track_config &cfg);

    /*
     * pts_us is relative to the segment start and must not go backwards. duration_us
     * is used for the final frame's end (file Duration).
     * Returns 0 written, 1 skipped (H.264 waiting for keyframe + SPS/PPS), negative errno.
     */
    int write_frame(const uint8_t *data, size_t size, int64_t pts_us, int64_t duration_us,
                    bool key);

    /* Finishes the file. Safe to call when not open. Returns 0 or negative errno. */
    int close();

    [[nodiscard]] bool is_open() const { return opened_; }

    [[nodiscard]] uint64_t frames_written() const { return frames_; }

private:
    struct cue_s
    {
        int64_t  time_ms = 0;
        uint64_t cluster_pos = 0; /* relative to segment data start */
    };

    int  write_header();
    int  start_cluster(int64_t ts_ms, bool key);
    int  finish_cluster();
    int  write_cues();
    int  write_seekhead(bool with_cues);
    int  patch_size(uint64_t size_pos, uint64_t size);
    void note_h264_params(const uint8_t *data, size_t size);
    [[nodiscard]] std::vector<uint8_t> build_avcc() const;

    [[nodiscard]] bool put(const void *p, size_t n);
    [[nodiscard]] int64_t tell() const;

    std::string      path_;
    FILE            *f_ = nullptr;
    mkv_track_config cfg_ {};
    bool             opened_ = false;
    bool             header_written_ = false;
    bool             io_error_ = false;

    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;

    uint64_t segment_size_pos_ = 0;
    uint64_t segment_data_pos_ = 0;
    uint64_t seekhead_pos_ = 0;
    uint64_t info_pos_ = 0;
    uint64_t duration_pos_ = 0;
    uint64_t tracks_pos_ = 0;
    uint64_t cues_pos_ = 0;

    bool     cluster_open_ = false;
    uint64_t cluster_size_pos_ = 0;
    int64_t  cluster_ts_ms_ = 0;

    int64_t last_pts_ms_ = -1;
    int64_t end_ms_ = 0;
    uint64_t frames_ = 0;

    std::vector<cue_s> cues_;
};

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_MKV_WRITER_HPP
