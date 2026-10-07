#ifndef VSTREAMER_CORE_H264_SPS_HPP
#define VSTREAMER_CORE_H264_SPS_HPP

#include <cstddef>
#include <cstdint>

namespace vstreamer
{

/* Best-effort SPS width/height from Annex-B buffer (first SPS NAL). */
[[nodiscard]] bool h264_annexb_sps_dimensions(const uint8_t *data, size_t size, int32_t *width,
                                            int32_t *height);

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_H264_SPS_HPP
