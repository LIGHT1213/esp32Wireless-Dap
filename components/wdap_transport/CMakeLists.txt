idf_component_register(
    SRCS "src/wdap_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash esp_timer esp_hw_support wdap_protocol board_config
)
