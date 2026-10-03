#ifndef VSTREAMER_TEST_APP_SELF_TEST_HPP
#define VSTREAMER_TEST_APP_SELF_TEST_HPP

#include "test_app/stream_sdl/channel_controller.hpp"
#include "test_app/stream_sdl/encoder_types.hpp"

#include "components/components.hpp"

namespace vstreamer::test_app
{

int send_channel_console(int console_port, const char *line);

bool run_self_test(h264_encoder_t &enc, vstreamer::component_sink *preview,
                   vstreamer::stream_receiver &rcv, vstreamer::stream_sender &sender,
                   int console_port, channel_controller *channel);

}  // namespace vstreamer::test_app

#endif
