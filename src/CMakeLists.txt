idf_component_register(SRCS "main.c" "frame_parser.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_netif nvs_flash)
