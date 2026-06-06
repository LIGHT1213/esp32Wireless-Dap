idf_component_register(
    SRCS "src/wdap_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_hw_support board_config
)
