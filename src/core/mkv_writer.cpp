#include "core/mkv_writer.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <unistd.h>

namespace vstreamer
{
namespace
{

using bytes = std::vector<uint8_t>;

/* Matroska element IDs (marker bits included). */
constexpr uint32_t k_ebml = 0x1A45DFA3;
constexpr uint32_t k_ebml_version = 0x4286;
constexpr uint32_t k_ebml_read_version = 0x42F7;
constexpr uint32_t k_ebml_max_id_length = 0x42F2;
constexpr uint32_t k_ebml_max_size_length = 0x42F3;
constexpr uint32_t k_doc_type = 0x4282;
constexpr uint32_t k_doc_type_version = 0x4287;
constexpr uint32_t k_doc_type_read_version = 0x4285;
constexpr uint32_t k_segment = 0x18538067;
constexpr uint32_t k_seek_head = 0x114D9B74;
constexpr uint32_t k_seek = 0x4DBB;
constexpr uint32_t k_seek_id = 0x53AB;
constexpr uint32_t k_seek_position = 0x53AC;
constexpr uint32_t k_info = 0x1549A966;
constexpr uint32_t k_timestamp_scale = 0x2AD7B1;
constexpr uint32_t k_duration = 0x4489;
constexpr uint32_t k_muxing_app = 0x4D80;
constexpr uint32_t k_writing_app = 0x5741;
constexpr uint32_t k_tracks = 0x1654AE6B;
constexpr uint32_t k_track_entry = 0xAE;
constexpr uint32_t k_track_number = 0xD7;
constexpr uint32_t k_track_uid = 0x73C5;
constexpr uint32_t k_track_type = 0x83;
constexpr uint32_t k_flag_lacing = 0x9C;
constexpr uint32_t k_default_duration = 0x23E383;
constexpr uint32_t k_codec_id = 0x86;
constexpr uint32_t k_codec_private = 0x63A2;
constexpr uint32_t k_video = 0xE0;
constexpr uint32_t k_pixel_width = 0xB0;
constexpr uint32_t k_pixel_height = 0xBA;
constexpr uint32_t k_cluster = 0x1F43B675;
constexpr uint32_t k_timestamp = 0xE7;
constexpr uint32_t k_simple_block = 0xA3;
constexpr uint32_t k_cues = 0x1C53BB6B;
constexpr uint32_t k_cue_point = 0xBB;
constexpr uint32_t k_cue_time = 0xB3;
constexpr uint32_t k_cue_track_positions = 0xB7;
constexpr uint32_t k_cue_track = 0xF7;
constexpr uint32_t k_cue_cluster_position = 0xF1;
constexpr uint32_t k_void = 0xEC;

/* Timestamps in ms; SimpleBlock offsets are int16, so clusters must stay under ~32 s. */
constexpr uint64_t k_timestamp_scale_ns = 1000000;
constexpr int64_t  k_cluster_target_ms = 1000;
constexpr int64_t  k_cluster_max_ms = 30000;

/* SeekHead with three entries (fixed 8-byte positions) is 68 bytes; the rest is Void. */
constexpr size_t k_seek_head_region = 96;
/* Duration element: 2-byte ID + 1-byte size + 8-byte float. A Void of the same length
 * holds its place until close(). */
constexpr size_t k_duration_element_len = 11;

constexpr uint8_t k_track_number_value = 1;

void put_id(bytes *b, uint32_t id)
{
    int n = 1;
    if (id > 0xFFFFFF)
    {
        n = 4;
    }
    else if (id > 0xFFFF)
    {
        n = 3;
    }
    else if (id > 0xFF)
    {
        n = 2;
    }
    for (int i = n - 1; i >= 0; --i)
    {
        b->push_back(static_cast<uint8_t>(id >> (8 * i)));
    }
}

void put_size_fixed(bytes *b, uint64_t size, int len)
{
    for (int i = len - 1; i >= 0; --i)
    {
        uint8_t byte = static_cast<uint8_t>(size >> (8 * i));
        if (i == len - 1)
        {
            byte = static_cast<uint8_t>((size >> (8 * i)) & (0xFFu >> len)) |
                   static_cast<uint8_t>(0x80u >> (len - 1));
        }
        b->push_back(byte);
    }
}

void put_size(bytes *b, uint64_t size)
{
    /* All-ones is reserved for "unknown", hence < 2^(7n) - 1. */
    int n = 1;
    while (n < 8 && size >= (uint64_t {1} << (7 * n)) - 1)
    {
        ++n;
    }
    put_size_fixed(b, size, n);
}

void el_uint(bytes *b, uint32_t id, uint64_t v)
{
    int n = 1;
    while (n < 8 && (v >> (8 * n)) != 0)
    {
        ++n;
    }
    put_id(b, id);
    put_size(b, static_cast<uint64_t>(n));
    for (int i = n - 1; i >= 0; --i)
    {
        b->push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

void el_uint_fixed8(bytes *b, uint32_t id, uint64_t v)
{
    put_id(b, id);
    put_size(b, 8);
    for (int i = 7; i >= 0; --i)
    {
        b->push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

void el_float(bytes *b, uint32_t id, double v)
{
    uint64_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    put_id(b, id);
    put_size(b, 8);
    for (int i = 7; i >= 0; --i)
    {
        b->push_back(static_cast<uint8_t>(u >> (8 * i)));
    }
}

void el_bin(bytes *b, uint32_t id, const uint8_t *data, size_t n)
{
    put_id(b, id);
    put_size(b, n);
    b->insert(b->end(), data, data + n);
}

void el_str(bytes *b, uint32_t id, const char *s)
{
    el_bin(b, id, reinterpret_cast<const uint8_t *>(s), std::strlen(s));
}

void el_master(bytes *b, uint32_t id, const bytes &body)
{
    put_id(b, id);
    put_size(b, body.size());
    b->insert(b->end(), body.begin(), body.end());
}

/* Void element of exactly total_len bytes (>= 2). */
void el_void(bytes *b, size_t total_len)
{
    put_id(b, k_void);
    if (total_len - 2 < 127)
    {
        put_size_fixed(b, total_len - 2, 1);
        b->insert(b->end(), total_len - 2, 0);
    }
    else
    {
        put_size_fixed(b, total_len - 9, 8);
        b->insert(b->end(), total_len - 9, 0);
    }
}

/*
 * Calls fn(nal, len) for each NAL of an Annex-B buffer. Trailing zero bytes before a
 * start code are not part of the NAL.
 */
template <typename Fn>
void for_each_nal(const uint8_t *data, size_t size, Fn fn)
{
    size_t i = 0;
    size_t nal_start = SIZE_MAX;
    while (i + 3 <= size)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            if (nal_start != SIZE_MAX)
            {
                size_t end = i;
                while (end > nal_start && data[end - 1] == 0)
                {
                    --end;
                }
                if (end > nal_start)
                {
                    fn(data + nal_start, end - nal_start);
                }
            }
            i += 3;
            nal_start = i;
            continue;
        }
        ++i;
    }
    if (nal_start != SIZE_MAX && nal_start < size)
    {
        fn(data + nal_start, size - nal_start);
    }
}

constexpr uint8_t k_nal_idr = 5;
constexpr uint8_t k_nal_sps = 7;
constexpr uint8_t k_nal_pps = 8;
constexpr uint8_t k_nal_aud = 9;

}  // namespace

mkv_writer::~mkv_writer()
{
    (void)close();
}

int mkv_writer::open(const std::string &path, const mkv_track_config &cfg)
{
    (void)close();
    if (path.empty() || cfg.width <= 0 || cfg.height <= 0)
    {
        return -EINVAL;
    }
    path_ = path;
    cfg_ = cfg;
    if (cfg_.fps <= 0)
    {
        cfg_.fps = 30;
    }
    opened_ = true;
    return 0;
}

bool mkv_writer::put(const void *p, size_t n)
{
    if (io_error_)
    {
        return false;
    }
    if (n > 0 && std::fwrite(p, 1, n, f_) != n)
    {
        io_error_ = true;
        return false;
    }
    return true;
}

int64_t mkv_writer::tell() const
{
    return static_cast<int64_t>(ftello(f_));
}

void mkv_writer::note_h264_params(const uint8_t *data, size_t size)
{
    for_each_nal(data, size, [this](const uint8_t *nal, size_t len) {
        const uint8_t type = nal[0] & 0x1F;
        if (type == k_nal_sps && len >= 4)
        {
            sps_.assign(nal, nal + len);
        }
        else if (type == k_nal_pps)
        {
            pps_.assign(nal, nal + len);
        }
    });
}

std::vector<uint8_t> mkv_writer::build_avcc() const
{
    /* AVCDecoderConfigurationRecord: one SPS, one PPS, 4-byte NAL lengths. */
    bytes b;
    b.push_back(1);
    b.push_back(sps_[1]);
    b.push_back(sps_[2]);
    b.push_back(sps_[3]);
    b.push_back(0xFF);
    b.push_back(0xE1);
    b.push_back(static_cast<uint8_t>(sps_.size() >> 8));
    b.push_back(static_cast<uint8_t>(sps_.size()));
    b.insert(b.end(), sps_.begin(), sps_.end());
    b.push_back(1);
    b.push_back(static_cast<uint8_t>(pps_.size() >> 8));
    b.push_back(static_cast<uint8_t>(pps_.size()));
    b.insert(b.end(), pps_.begin(), pps_.end());
    return b;
}

int mkv_writer::write_seekhead(bool with_cues)
{
    bytes body;
    auto  seek = [&body, this](uint32_t id, uint64_t pos) {
        bytes s;
        bytes idb;
        put_id(&idb, id);
        el_bin(&s, k_seek_id, idb.data(), idb.size());
        el_uint_fixed8(&s, k_seek_position, pos - segment_data_pos_);
        el_master(&body, k_seek, s);
    };
    seek(k_info, info_pos_);
    seek(k_tracks, tracks_pos_);
    if (with_cues)
    {
        seek(k_cues, cues_pos_);
    }
    bytes b;
    el_master(&b, k_seek_head, body);
    el_void(&b, k_seek_head_region - b.size());

    const int64_t end = tell();
    if (fseeko(f_, static_cast<off_t>(seekhead_pos_), SEEK_SET) != 0)
    {
        return -EIO;
    }
    const bool ok = put(b.data(), b.size());
    if (end >= 0 && end > static_cast<int64_t>(seekhead_pos_ + b.size()))
    {
        (void)fseeko(f_, static_cast<off_t>(end), SEEK_SET);
    }
    return ok ? 0 : -EIO;
}

int mkv_writer::write_header()
{
    f_ = std::fopen(path_.c_str(), "wb");
    if (nullptr == f_)
    {
        return -errno;
    }

    bytes b;
    {
        bytes h;
        el_uint(&h, k_ebml_version, 1);
        el_uint(&h, k_ebml_read_version, 1);
        el_uint(&h, k_ebml_max_id_length, 4);
        el_uint(&h, k_ebml_max_size_length, 8);
        el_str(&h, k_doc_type, "matroska");
        el_uint(&h, k_doc_type_version, 4);
        el_uint(&h, k_doc_type_read_version, 2);
        el_master(&b, k_ebml, h);
    }

    /* Segment of unknown size; patched in close(). */
    put_id(&b, k_segment);
    segment_size_pos_ = b.size();
    b.push_back(0x01);
    b.insert(b.end(), 7, 0xFF);
    segment_data_pos_ = b.size();

    seekhead_pos_ = b.size();
    b.insert(b.end(), k_seek_head_region, 0); /* filled by write_seekhead() below */

    info_pos_ = b.size();
    {
        bytes info;
        el_uint(&info, k_timestamp_scale, k_timestamp_scale_ns);
        el_str(&info, k_muxing_app, "vstreamer");
        el_str(&info, k_writing_app, "vstreamer");
        const size_t dur_off = info.size();
        el_void(&info, k_duration_element_len);
        el_master(&b, k_info, info);
        duration_pos_ = b.size() - info.size() + dur_off;
    }

    tracks_pos_ = b.size();
    {
        bytes entry;
        el_uint(&entry, k_track_number, k_track_number_value);
        const auto uid = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        el_uint(&entry, k_track_uid, (uid & 0xFFFFFFFFFFFFull) | 1);
        el_uint(&entry, k_track_type, 1);
        el_uint(&entry, k_flag_lacing, 0);
        el_uint(&entry, k_default_duration, 1000000000ull / static_cast<uint64_t>(cfg_.fps));
        if (cfg_.codec == mkv_codec_e::H264)
        {
            el_str(&entry, k_codec_id, "V_MPEG4/ISO/AVC");
            const bytes avcc = build_avcc();
            el_bin(&entry, k_codec_private, avcc.data(), avcc.size());
        }
        else
        {
            el_str(&entry, k_codec_id, "V_MJPEG");
        }
        bytes video;
        el_uint(&video, k_pixel_width, static_cast<uint64_t>(cfg_.width));
        el_uint(&video, k_pixel_height, static_cast<uint64_t>(cfg_.height));
        el_master(&entry, k_video, video);

        bytes tracks;
        el_master(&tracks, k_track_entry, entry);
        el_master(&b, k_tracks, tracks);
    }

    if (!put(b.data(), b.size()) || write_seekhead(false) < 0 || std::fflush(f_) != 0)
    {
        /* Close it, so the next keyframe starts over with a fresh fopen() instead of leaking
         * this FILE. */
        (void)std::fclose(f_);
        f_ = nullptr;
        io_error_ = false;
        return -EIO;
    }
    header_written_ = true;
    return 0;
}

int mkv_writer::patch_size(uint64_t size_pos, uint64_t size)
{
    bytes b;
    put_size_fixed(&b, size, 8);
    const int64_t end = tell();
    if (end < 0 || fseeko(f_, static_cast<off_t>(size_pos), SEEK_SET) != 0)
    {
        return -EIO;
    }
    const bool ok = put(b.data(), b.size());
    if (fseeko(f_, static_cast<off_t>(end), SEEK_SET) != 0)
    {
        return -EIO;
    }
    return ok ? 0 : -EIO;
}

int mkv_writer::finish_cluster()
{
    if (!cluster_open_)
    {
        return 0;
    }
    cluster_open_ = false;
    const int64_t end = tell();
    if (end < 0)
    {
        return -EIO;
    }
    const uint64_t data_start = cluster_size_pos_ + 8;
    int            r = patch_size(cluster_size_pos_, static_cast<uint64_t>(end) - data_start);
    if (std::fflush(f_) != 0)
    {
        r = -EIO;
    }
    /* On FATFS the directory entry (file length) only reaches the card on sync. */
    (void)fsync(fileno(f_));
    return r;
}

int mkv_writer::start_cluster(int64_t ts_ms, bool key)
{
    int r = finish_cluster();
    if (r < 0)
    {
        return r;
    }
    const int64_t pos = tell();
    if (pos < 0)
    {
        return -EIO;
    }
    if (key)
    {
        cues_.push_back(cue_s {ts_ms, static_cast<uint64_t>(pos) - segment_data_pos_});
    }
    bytes b;
    put_id(&b, k_cluster);
    cluster_size_pos_ = static_cast<uint64_t>(pos) + b.size();
    b.push_back(0x01);
    b.insert(b.end(), 7, 0xFF);
    el_uint(&b, k_timestamp, static_cast<uint64_t>(ts_ms));
    if (!put(b.data(), b.size()))
    {
        return -EIO;
    }
    cluster_open_ = true;
    cluster_ts_ms_ = ts_ms;
    return 0;
}

int mkv_writer::write_frame(const uint8_t *data, size_t size, int64_t pts_us,
                            int64_t duration_us, bool key)
{
    if (!opened_)
    {
        return -EBADF;
    }
    if (nullptr == data || 0 == size)
    {
        return -EINVAL;
    }
    if (io_error_)
    {
        return -EIO;
    }

    const bool h264 = cfg_.codec == mkv_codec_e::H264;
    size_t     payload = size;
    if (h264)
    {
        note_h264_params(data, size);
        payload = 0;
        bool idr = false;
        for_each_nal(data, size, [&payload, &idr](const uint8_t *nal, size_t len) {
            const uint8_t type = nal[0] & 0x1F;
            if (type == k_nal_idr)
            {
                idr = true;
            }
            if (type != k_nal_aud)
            {
                payload += 4 + len;
            }
        });
        if (0 == payload)
        {
            return -EINVAL;
        }
        key = key || idr;
        if (!header_written_ && (!key || sps_.size() < 4 || pps_.empty()))
        {
            return 1;
        }
    }
    else
    {
        key = true;
    }

    if (!header_written_)
    {
        const int r = write_header();
        if (r < 0)
        {
            return r;
        }
    }

    int64_t pts_ms = pts_us > 0 ? (pts_us + 500) / 1000 : 0;
    if (last_pts_ms_ >= 0 && pts_ms < last_pts_ms_)
    {
        pts_ms = last_pts_ms_;
    }

    const int64_t in_cluster = pts_ms - cluster_ts_ms_;
    if (!cluster_open_ || (key && in_cluster >= k_cluster_target_ms) ||
        in_cluster > k_cluster_max_ms)
    {
        const int r = start_cluster(pts_ms, key);
        if (r < 0)
        {
            return r;
        }
    }

    const auto rel = static_cast<int16_t>(pts_ms - cluster_ts_ms_);
    bytes      hdr;
    put_id(&hdr, k_simple_block);
    put_size(&hdr, 4 + payload);
    hdr.push_back(0x80 | k_track_number_value);
    hdr.push_back(static_cast<uint8_t>(static_cast<uint16_t>(rel) >> 8));
    hdr.push_back(static_cast<uint8_t>(rel));
    hdr.push_back(key ? 0x80 : 0x00);
    if (!put(hdr.data(), hdr.size()))
    {
        return -EIO;
    }

    if (h264)
    {
        for_each_nal(data, size, [this](const uint8_t *nal, size_t len) {
            if ((nal[0] & 0x1F) == k_nal_aud)
            {
                return;
            }
            const uint8_t len_be[4] = {
                static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
            (void)put(len_be, sizeof(len_be));
            (void)put(nal, len);
        });
    }
    else
    {
        (void)put(data, size);
    }
    if (io_error_)
    {
        return -EIO;
    }

    int64_t dur_ms = (duration_us + 500) / 1000;
    if (dur_ms <= 0)
    {
        dur_ms = 1000 / cfg_.fps;
    }
    if (pts_ms + dur_ms > end_ms_)
    {
        end_ms_ = pts_ms + dur_ms;
    }
    last_pts_ms_ = pts_ms;
    ++frames_;
    return 0;
}

int mkv_writer::write_cues()
{
    bytes body;
    for (const cue_s &c : cues_)
    {
        bytes pos;
        el_uint(&pos, k_cue_track, k_track_number_value);
        el_uint(&pos, k_cue_cluster_position, c.cluster_pos);
        bytes point;
        el_uint(&point, k_cue_time, static_cast<uint64_t>(c.time_ms));
        el_master(&point, k_cue_track_positions, pos);
        el_master(&body, k_cue_point, point);
    }
    bytes b;
    el_master(&b, k_cues, body);
    return put(b.data(), b.size()) ? 0 : -EIO;
}

int mkv_writer::close()
{
    if (!opened_)
    {
        return 0;
    }
    int r = 0;
    if (nullptr != f_)
    {
        if (header_written_ && !io_error_)
        {
            r = finish_cluster();
            const bool have_cues = !cues_.empty();
            if (have_cues)
            {
                const int64_t pos = tell();
                cues_pos_ = pos < 0 ? 0 : static_cast<uint64_t>(pos);
                if (write_cues() < 0)
                {
                    r = -EIO;
                }
            }
            const int64_t end = tell();

            bytes dur;
            el_float(&dur, k_duration, static_cast<double>(end_ms_));
            if (fseeko(f_, static_cast<off_t>(duration_pos_), SEEK_SET) != 0 ||
                !put(dur.data(), dur.size()))
            {
                r = -EIO;
            }
            if (write_seekhead(have_cues) < 0)
            {
                r = -EIO;
            }
            if (end < 0 || fseeko(f_, static_cast<off_t>(end), SEEK_SET) != 0 ||
                patch_size(segment_size_pos_, static_cast<uint64_t>(end) - segment_data_pos_) < 0)
            {
                r = -EIO;
            }
        }
        if (std::fflush(f_) != 0)
        {
            r = -EIO;
        }
        (void)fsync(fileno(f_));
        if (std::fclose(f_) != 0)
        {
            r = -EIO;
        }
        f_ = nullptr;
    }
    if (io_error_ && 0 == r)
    {
        r = -EIO;
    }

    opened_ = false;
    header_written_ = false;
    io_error_ = false;
    path_.clear();
    sps_.clear();
    pps_.clear();
    cluster_open_ = false;
    cluster_ts_ms_ = 0;
    last_pts_ms_ = -1;
    end_ms_ = 0;
    frames_ = 0;
    cues_.clear();
    return r;
}

}  // namespace vstreamer
