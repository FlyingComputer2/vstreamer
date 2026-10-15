// Creates each vstreamer component the IDF build provides and prints its name, so the
// image links the whole library.

#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"

#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include <cstdio>
#include <string>

namespace
{

template <typename T>
void report(const char *type, const T &c)
{
    std::printf("%s: %s\n", type, c ? c->name().c_str() : "not available");
}

}  // namespace

extern "C" void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    using vstreamer::component_factory;
    report("stream_receiver", component_factory::create_source("stream_receiver"));
    report("rtp_h264_pay", component_factory::create_coder("rtp_h264_pay"));
    report("rtp_h264_depay", component_factory::create_coder("rtp_h264_depay"));
    report("stream_sender", component_factory::create_sink("stream_sender"));
    report("mkv_sink", component_factory::create_sink("mkv_sink"));
}
