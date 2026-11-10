idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_event esp_wifi esp_netif)
