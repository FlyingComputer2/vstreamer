#include "ethernet_init.hpp"

#include <cstdio>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace
{

constexpr int k_eth_phy_addr = 1;
constexpr int k_eth_phy_reset_gpio = 51;

const char *k_tag = "vstreamer_eth";

SemaphoreHandle_t s_got_ip;
esp_netif_t      *s_netif;

void on_got_ip(void *, esp_event_base_t, int32_t, void *data)
{
    const auto *ev = static_cast<const ip_event_got_ip_t *>(data);
    std::printf("P4_IP ip=" IPSTR " gw=" IPSTR "\n", IP2STR(&ev->ip_info.ip),
                IP2STR(&ev->ip_info.gw));
    std::fflush(stdout);
    if (nullptr != s_got_ip)
    {
        xSemaphoreGive(s_got_ip);
    }
}

void on_eth(void *, esp_event_base_t, int32_t id, void *)
{
    if (ETHERNET_EVENT_CONNECTED == id)
    {
        ESP_LOGI(k_tag, "link up");
    }
    else if (ETHERNET_EVENT_DISCONNECTED == id)
    {
        ESP_LOGW(k_tag, "link down");
    }
}

}  // namespace

esp_err_t vstreamer_idf_ethernet_start()
{
    s_got_ip = xSemaphoreCreateBinary();
    if (nullptr == s_got_ip)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    if (nullptr == s_netif)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_set_hostname(s_netif, "vstreamer-p4");

    eth_mac_config_t        mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_phy_config_t        phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = k_eth_phy_addr;
    phy_cfg.reset_gpio_num = k_eth_phy_reset_gpio;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (nullptr == mac || nullptr == phy)
    {
        return ESP_FAIL;
    }

    esp_eth_config_t   eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t   eth = nullptr;
    const esp_err_t    err = esp_eth_driver_install(&eth_cfg, &eth);
    if (ESP_OK != err)
    {
        return err;
    }
    esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, nullptr));
    return esp_eth_start(eth);
}

esp_err_t vstreamer_idf_wait_for_ip(int timeout_ms)
{
    if (nullptr == s_got_ip)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (pdTRUE == xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(timeout_ms)))
    {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
