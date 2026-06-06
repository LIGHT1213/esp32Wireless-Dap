idf_component_register(
    SRCS "frontend_main.c"
    INCLUDE_DIRS "."
    REQUIRES board_config wdap_transport usb_dap_frontend
)
