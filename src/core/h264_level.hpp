#ifndef VSTREAMER_CORE_H264_LEVEL_HPP
#define VSTREAMER_CORE_H264_LEVEL_HPP

namespace vstreamer
{

/* H.264 High profile level_idc (Annex A). kbps=0 ignores MaxBR. */
int h264_level_for_size(int w, int h, int fps, int kbps);

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_H264_LEVEL_HPP
