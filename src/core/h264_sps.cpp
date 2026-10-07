#include "core/h264_sps.hpp"

namespace vstreamer
{
namespace
{

struct bit_reader
{
    const uint8_t *data = nullptr;
    size_t         size = 0;
    size_t         bit_pos = 0;

    [[nodiscard]] bool read_bit(bool *out)
    {
        if (nullptr == out || bit_pos >= size * 8)
        {
            return false;
        }
        const size_t byte_i = bit_pos / 8;
        const unsigned shift = 7U - static_cast<unsigned>(bit_pos % 8);
        *out = ((data[byte_i] >> shift) & 1U) != 0;
        bit_pos++;
        return true;
    }

    [[nodiscard]] bool read_bits(unsigned n, uint32_t *out)
    {
        if (nullptr == out)
        {
            return false;
        }
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            bool b = false;
            if (!read_bit(&b))
            {
                return false;
            }
            v = (v << 1) | (b ? 1U : 0U);
        }
        *out = v;
        return true;
    }

    [[nodiscard]] bool read_ue(uint32_t *out)
    {
        if (nullptr == out)
        {
            return false;
        }
        unsigned zeros = 0;
        while (true)
        {
            bool b = false;
            if (!read_bit(&b))
            {
                return false;
            }
            if (b)
            {
                break;
            }
            zeros++;
            if (zeros > 31)
            {
                return false;
            }
        }
        uint32_t val = 0;
        if (zeros > 0)
        {
            if (!read_bits(zeros, &val))
            {
                return false;
            }
        }
        *out = (1U << zeros) - 1U + val;
        return true;
    }
};

[[nodiscard]] const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end)
{
    while (p + 3 < end)
    {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
        {
            return p;
        }
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
        {
            return p;
        }
        ++p;
    }
    return end;
}

[[nodiscard]] bool parse_sps_rbsp(const uint8_t *rbsp, size_t rbsp_len, int32_t *width,
                                  int32_t *height)
{
    if (nullptr == rbsp || nullptr == width || nullptr == height || rbsp_len < 4)
    {
        return false;
    }
    bit_reader br {rbsp, rbsp_len};
    uint32_t   profile_idc = 0;
    if (!br.read_bits(8, &profile_idc))
    {
        return false;
    }
    uint32_t tmp = 0;
    if (!br.read_bits(8, &tmp))
    {
        return false;
    }
    if (!br.read_ue(&tmp))
    {
        return false;
    }
    uint32_t level_idc = 0;
    if (!br.read_bits(8, &level_idc))
    {
        return false;
    }
    if (!br.read_ue(&tmp))
    {
        return false;
    }
    if (100 == profile_idc || 110 == profile_idc || 122 == profile_idc || 244 == profile_idc ||
        44 == profile_idc || 83 == profile_idc || 86 == profile_idc || 118 == profile_idc ||
        128 == profile_idc || 138 == profile_idc || 139 == profile_idc || 134 == profile_idc)
    {
        uint32_t chroma = 0;
        if (!br.read_ue(&chroma))
        {
            return false;
        }
        if (chroma == 3)
        {
            if (!br.read_bit(nullptr))
            {
                return false;
            }
        }
        if (!br.read_ue(&tmp) || !br.read_ue(&tmp) || !br.read_bit(nullptr))
        {
            return false;
        }
        bool seq_scaling = false;
        if (!br.read_bit(&seq_scaling))
        {
            return false;
        }
        if (seq_scaling)
        {
            for (int i = 0; i < ((chroma != 3) ? 8 : 12); ++i)
            {
                bool present = false;
                if (!br.read_bit(&present))
                {
                    return false;
                }
                if (present)
                {
                    int last = 8;
                    int next = 8;
                    for (int j = 0; j < 64;)
                    {
                        uint32_t delta = 0;
                        if (!br.read_ue(&delta))
                        {
                            return false;
                        }
                        next = (last + static_cast<int>(delta) + 256) % 256;
                        if (0 == delta)
                        {
                            break;
                        }
                        last = next;
                        ++j;
                    }
                }
            }
        }
    }
    if (!br.read_ue(&tmp))
    {
        return false;
    }
    uint32_t pic_order_cnt_type = tmp;
    if (0 == pic_order_cnt_type)
    {
        if (!br.read_ue(&tmp))
        {
            return false;
        }
    }
    else if (1 == pic_order_cnt_type)
    {
        if (!br.read_bit(nullptr) || !br.read_ue(&tmp) || !br.read_ue(&tmp))
        {
            return false;
        }
        uint32_t cycles = 0;
        if (!br.read_ue(&cycles))
        {
            return false;
        }
        for (uint32_t i = 0; i < cycles; ++i)
        {
            if (!br.read_ue(&tmp))
            {
                return false;
            }
        }
    }
    else if (pic_order_cnt_type > 2)
    {
        return false;
    }
    if (!br.read_ue(&tmp))
    {
        return false;
    }
    uint32_t log2_minus4 = 0;
    if (!br.read_ue(&log2_minus4))
    {
        return false;
    }
    uint32_t pic_order_present = 0;
    if (!br.read_ue(&pic_order_present))
    {
        return false;
    }
    uint32_t pic_width_in_mbs = 0;
    if (!br.read_ue(&pic_width_in_mbs))
    {
        return false;
    }
    uint32_t pic_height_in_map_units = 0;
    if (!br.read_ue(&pic_height_in_map_units))
    {
        return false;
    }
    bool frame_mbs_only = false;
    if (!br.read_bit(&frame_mbs_only))
    {
        return false;
    }
    if (!frame_mbs_only)
    {
        if (!br.read_bit(nullptr))
        {
            return false;
        }
    }
    if (!br.read_bit(nullptr))
    {
        return false;
    }
    uint32_t crop_left = 0;
    uint32_t crop_right = 0;
    uint32_t crop_top = 0;
    uint32_t crop_bottom = 0;
    bool     frame_cropping = false;
    if (!br.read_bit(&frame_cropping))
    {
        return false;
    }
    if (frame_cropping)
    {
        if (!br.read_ue(&crop_left) || !br.read_ue(&crop_right) || !br.read_ue(&crop_top) ||
            !br.read_ue(&crop_bottom))
        {
            return false;
        }
    }
    const int w =
        static_cast<int>((pic_width_in_mbs + 1U) * 16U - (crop_left + crop_right) * 2U);
    const int h = static_cast<int>((2U - (frame_mbs_only ? 1U : 0U)) * (pic_height_in_map_units + 1U) *
                                     16U -
                                 (crop_top + crop_bottom) * 2U);
    if (w <= 0 || h <= 0)
    {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

}  // namespace

bool h264_annexb_sps_dimensions(const uint8_t *data, size_t size, int32_t *width, int32_t *height)
{
    if (nullptr == data || 0 == size)
    {
        return false;
    }
    const uint8_t *begin = data;
    const uint8_t *end = data + size;
    const uint8_t *sc = find_start_code(begin, end);
    while (sc < end)
    {
        size_t sc_len = 3;
        if (sc + 3 < end && sc[2] == 0 && sc[3] == 1)
        {
            sc_len = 4;
        }
        const uint8_t *nal = sc + sc_len;
        if (nal >= end)
        {
            break;
        }
        const int nal_type = nal[0] & 0x1f;
        if (7 == nal_type)
        {
            return parse_sps_rbsp(nal + 1, static_cast<size_t>(end - nal - 1), width, height);
        }
        sc = find_start_code(nal + 1, end);
    }
    return false;
}

}  // namespace vstreamer
