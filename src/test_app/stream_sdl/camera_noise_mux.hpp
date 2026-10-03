#ifndef VSTREAMER_TEST_APP_CAMERA_NOISE_MUX_HPP
#define VSTREAMER_TEST_APP_CAMERA_NOISE_MUX_HPP

#include "components/components_config.hpp"

#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_NOISE_SOURCE)

#include <string>
#include <string_view>

#include "components/noise_source.hpp"
#include "components/v4l2_source.hpp"
#include "core/component_source.hpp"

namespace vstreamer::test_app
{

/* Pipeline creator policy: UVC capture with NV12 noise when the camera returns -ENODEV. */
class camera_noise_mux_source : public component_source
{
public:
    camera_noise_mux_source(v4l2_source &camera, noise_source &noise);

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] media_kind_e output_kind() const override;

    int  open() override;
    void close() override;

    int output(uint8_t port, data_packet &out, int timeout_ms) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

    [[nodiscard]] bool noise_fallback_active() const { return noise_active_; }

private:
    v4l2_source  &camera_;
    noise_source &noise_;
    bool                     noise_active_ = false;
    bool                     logged_noise_ = false;
    bool                     logged_camera_ = false;
};

}  // namespace vstreamer::test_app

#endif  // ENABLE_V4L2_SOURCE && ENABLE_NOISE_SOURCE

#endif  // VSTREAMER_TEST_APP_CAMERA_NOISE_MUX_HPP
