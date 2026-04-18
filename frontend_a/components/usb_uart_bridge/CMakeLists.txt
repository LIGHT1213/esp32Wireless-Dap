idf_component_register(
    SRCS "usb_uart_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos wifi_link wdap_common wdap_runtime espressif__esp_tinyusb
)
