idf_component_register(
    SRCS "backend_main.c"
    INCLUDE_DIRS "."
    REQUIRES board_config wdap_transport daplink_port backend_swd_gpio
)
