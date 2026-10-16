idf_component_register(SRCS "main.cpp" "ethernet_init.cpp"
                       PRIV_REQUIRES vstreamer esp_netif esp_event esp_eth esp_timer esp_mm nvs_flash
                                     mbedtls)
