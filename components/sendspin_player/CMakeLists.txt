idf_component_register(SRCS "sendspin_player.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES lightsnapcast network_interface nvs_flash
                                     esp_driver_i2s esp_timer esp_pm esp_wifi
                                     esp_app_format mdns event_log status_led)
