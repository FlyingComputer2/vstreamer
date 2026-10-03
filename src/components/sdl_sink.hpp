#ifndef VSTREAMER_COMPONENTS_SDL_SINK_HPP
#define VSTREAMER_COMPONENTS_SDL_SINK_HPP

#include "components/components_config.hpp"
#ifndef ENABLE_SDL_SINK
#error "sdl_sink requires -DENABLE_SDL_SINK=ON"
#endif

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "components/sdl_nv12_presenter.hpp"
#include "core/component_sink.hpp"
#include "core/data_packet.hpp"

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
    [[nodiscard]] media_kind_e input_kind() const override;

    int  open() override;
    void close() override;

    int prepare(int width, int height);

    int input(uint8_t port, const data_packet &in) override;

    int configure(std::string_view key, std::string_view value) override;
    int query(std::string_view key, std::string *value) const override;

private:
    const char *presenter_video_driver() const;

    mutable std::mutex mu;

    bool opened = false;
    uint64_t frames_in = 0;
    std::string video_driver = "auto";

    std::optional<sdl_nv12_presenter> present;
};

}  // namespace vstreamer

#endif  // VSTREAMER_COMPONENTS_SDL_SINK_HPP
