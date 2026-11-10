idf_component_register(SRCS "event_log.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_system
                       PRIV_REQUIRES esp_timer esp_rom)
