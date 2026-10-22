#ifndef VSTREAMER_COMPONENTS_SDL_SINK_HPP
#define VSTREAMER_COMPONENTS_SDL_SINK_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_SDL_SINK
#error "sdl_sink requires -DENABLE_SDL_SINK=ON"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "components/sdl_nv12_presenter.hpp"
#include "core/component_sink.hpp"
#include "core/component_input.hpp"
#include "core/port_caps.hpp"
#include "core/sdu_caps.hpp"

namespace vstreamer
{

/* Packed NV12 preview sink. */
class sdl_sink : public component_sink
{
public:
    sdl_sink();
    ~sdl_sink() override;

    sdl_sink(const sdl_sink &) = delete;
    sdl_sink &operator=(const sdl_sink &) = delete;

    [[nodiscard]] std::string name() const override;
    int  open() override;
    void close() override;

    /* Queues the frame and presents it on the calling thread, which must be the render thread.
     * Caps PDUs only update the input caps. */
    int input(component_pdu &&in) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    int present_pending();
    const char *presenter_video_driver() const;

    [[nodiscard]] int input_pdu_locked(component_pdu &&in);
    [[nodiscard]] bool raw_caps_acceptable(const video_raw_caps &caps) const;
    [[nodiscard]] nv12_present_sample sample_from_pdu(const component_pdu &in) const;

    static const std::vector<port_desc> &input_ports();

    mutable std::mutex mu;

    std::atomic<bool> opened {false};
    std::atomic<bool> close_requested {false};
    uint64_t frames_in = 0;
    uint64_t dropped = 0;
    std::string video_driver = "auto";

    bool           have_input_caps_ = false;
    bool           caps_reject_ = false;
    video_raw_caps input_caps_ {};
    int            configured_w_ = 0;
    int            configured_h_ = 0;

    std::optional<sdl_nv12_presenter> present;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_SDL_SINK_HPP
