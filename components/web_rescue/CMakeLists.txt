idf_component_register(
    SRCS "web_rescue.c" "web_rescue_page.c" "rescue_mode.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi esp_netif esp_event
    PRIV_REQUIRES app_update spi_flash esp_partition efuse esp_timer nvs_flash
                  esp_driver_gpio bootloader_support esp_app_format json)
