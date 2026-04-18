idf_component_register(
    SRCS "uart_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_driver_uart wdap_common
)
