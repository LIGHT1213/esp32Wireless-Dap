idf_component_register(
    SRCS "src/backend_swd_gpio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_uart board_config
)
