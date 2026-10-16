#pragma once

#include "esp_err.h"

/* Waveshare ESP32-P4-WIFI6-DEV-KIT: IP101 PHY, same as rover-remote-esp32p4. */
esp_err_t vstreamer_idf_ethernet_start();
/* Blocks until IP_EVENT_ETH_GOT_IP or timeout_ms elapses. */
esp_err_t vstreamer_idf_wait_for_ip(int timeout_ms);
