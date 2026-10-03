/* rx_state.cpp — receive-half state. */

#include "apps/common/rx/rx_state.hpp"

#include <cerrno>

namespace vstreamer::apps::rx
{

rx_state g_rx;

#if defined(ENABLE_H264_DECODER_MPP)
int ensure_decoder_open(vstreamer::h264_decoder_mpp *dec)
{
    if (g_rx.skip_decode.load() || nullptr == dec)
    {
        return -EINVAL;
    }
    if (g_rx.dec_opened.load())
    {
        return 0;
    }
    const int r = dec->open();
    if (r < 0)
    {
        return r;
    }
    g_rx.dec_opened = true;
    return 0;
}
#endif

}  // namespace vstreamer::apps::rx
