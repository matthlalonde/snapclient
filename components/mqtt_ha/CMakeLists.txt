idf_component_register(SRCS "mqtt_ha.c" "mqtt_ha_json.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES mqtt settings_manager event_log esp_wifi
                                     esp_app_format esp_timer esp_hw_support)
