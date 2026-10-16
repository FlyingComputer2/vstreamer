// Loopback smoke test for the vstreamer component on ESP-IDF:
//
//   stream_sender (tx) -> UDP 127.0.0.1:5600 -> [pipeline: stream_receiver -> stream_sender]
//                      -> UDP 127.0.0.1:5601 -> stream_receiver (rx)
//
// Runs on a pthread (see app_main). Runs every thread the IDF build has (pipeline, send, telemetry, receive, mkv mux), counts
// the datagrams that arrive at rx and prints each thread's unused stack.

#include "core/component_factory.hpp"
#include "core/component_pdu.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "core/pipeline.hpp"
#include "core/shared_sized_buffer.hpp"
#include "core/thread_spawn.hpp"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace
{

constexpr int k_datagrams = 300;
constexpr int k_payload_bytes = 1000;

void print_heap(const char *when)
{
    std::printf("heap %-16s internal %u (largest %u), psram %u\n", when,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void print_stack_left(const char *task)
{
    TaskHandle_t h = xTaskGetHandle(task);
    if (nullptr == h)
    {
        std::printf("  %-12s not running\n", task);
        return;
    }
    std::printf("  %-12s %u bytes unused\n", task,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(h)));
}

void print_query(const char *who, const vstreamer::component &c, const char *key)
{
    std::string v;
    if (c.query(key, &v) == 0)
    {
        std::printf("  %s %s = %s\n", who, key, v.c_str());
    }
}

int run()
{
    using vstreamer::component_factory;

    print_heap("start");
    auto rx = component_factory::create_source("stream_receiver");
    print_heap("after rx");
    auto tx = component_factory::create_sink("stream_sender");
    print_heap("after tx");
    auto mkv = component_factory::create_sink("mkv_sink");
    print_heap("after mkv");
    if (!rx || !tx || !mkv)
    {
        std::printf("component missing from the build\n");
        return -1;
    }

    rx->configure("listen", "127.0.0.1:5601");
    if (rx->open() < 0)
    {
        std::printf("rx open failed\n");
        return -1;
    }

    vstreamer::pipeline relay("relay");
    relay.add_source("in", "stream_receiver");
    relay.add_sink("out", "stream_sender");
    relay.configure("in", "listen", "127.0.0.1:5600");
    relay.configure("out", "stream", "127.0.0.1:5601");
    relay.link("in", "out");
    /* stream_sender starts with its send gate closed. */
    static_cast<vstreamer::component_sink *>(relay.node("out"))->set_enabled(true, 0);
    relay.set_cpu(1);
    const int sr = relay.start();
    if (sr < 0)
    {
        std::printf("relay start failed: %d\n", sr);
        return -1;
    }

    tx->configure("stream", "127.0.0.1:5600");
    tx->configure("max_kbps", "20000");
    tx->set_enabled(true, 0);
    if (tx->open() < 0)
    {
        std::printf("tx open failed\n");
        return -1;
    }

    /* Starts the mux thread; nothing is recorded without an "output". */
    (void)mkv->open();

    uint8_t payload[k_payload_bytes];
    for (int i = 0; i < k_datagrams; ++i)
    {
        std::memset(payload, i & 0xFF, sizeof(payload));
        vstreamer::component_pdu pdu;
        pdu.sdu_type = vstreamer::sdu_type_e::STREAM_DGRAM;
        pdu.sdu = vstreamer::shared_sized_buffer::copy_from(payload, sizeof(payload));
        while (tx->input(std::move(pdu)) == -EAGAIN)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            pdu.sdu_type = vstreamer::sdu_type_e::STREAM_DGRAM;
            pdu.sdu = vstreamer::shared_sized_buffer::copy_from(payload, sizeof(payload));
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    int received = 0;
    for (int waited = 0; waited < 2000 && received < k_datagrams; waited += 10)
    {
        vstreamer::component_pdu out;
        while (rx->output(out) == 0)
        {
            ++received;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    std::printf("received %d / %d datagrams through the relay pipeline\n", received,
                k_datagrams);
    print_query("tx", *tx, "stats");
    for (const char *k : {"udp_packet_received", "fec_packet_received", "rx_truncated",
                          "rx_oversize", "rx_bad_header", "stats"})
    {
        print_query("relay.in", *relay.node("in"), k);
    }
    print_query("rx", *rx, "fec_packet_received");
    print_query("rx", *rx, "fec_recovered");

    std::printf("thread stacks (high-water mark):\n");
    for (const char *t : {"smoke", "vs_pipeline", "vs_send", "vs_telemetry", "vs_recv", "vs_mkv"})
    {
        print_stack_left(t);
    }

    mkv->close();
    tx->close();
    relay.stop();
    rx->close();
    std::printf("smoke test %s\n", received == k_datagrams ? "PASSED" : "FAILED");
    return received == k_datagrams ? 0 : -1;
}

}  // namespace

extern "C" void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    /* pipeline control uses std::this_thread::get_id(), which needs a pthread; app_main is a
     * plain FreeRTOS task. */
    auto t = vstreamer::spawn_thread(vstreamer::thread_opts {"smoke", 16 * 1024}, [] { (void)run(); });
    t.join();
}
