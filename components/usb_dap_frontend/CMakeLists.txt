idf_component_register(
    SRCS "src/usb_dap_frontend.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config wdap_transport espressif__esp_tinyusb
)
