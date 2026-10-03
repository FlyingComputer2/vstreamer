#ifndef VSTREAMER_TEST_APP_CAMERA_NOISE_MUX_LOGIC_HPP
#define VSTREAMER_TEST_APP_CAMERA_NOISE_MUX_LOGIC_HPP

#include "core/component_source.hpp"
#include "core/data_packet.hpp"

#include <cerrno>

namespace vstreamer::test_app
{

/* Shared output policy for camera_noise_mux_source (unit-tested with fake sources). */
inline int camera_noise_mux_output(component_source *camera, component_source *noise,
                                   bool *noise_active, uint8_t port, data_packet &out,
                                   int timeout_ms)
{
    if (nullptr == camera || nullptr == noise || nullptr == noise_active)
    {
        return -EINVAL;
    }
    const int cam = camera->output(port, out, timeout_ms);
    if (0 == cam)
    {
        *noise_active = false;
        return 0;
    }
    if (-ENODEV != cam)
    {
        return cam;
    }
    const int nr = noise->output(port, out, timeout_ms);
    if (0 == nr)
    {
        *noise_active = true;
    }
    return nr;
}

}  // namespace vstreamer::test_app

#endif  // VSTREAMER_TEST_APP_CAMERA_NOISE_MUX_LOGIC_HPP
