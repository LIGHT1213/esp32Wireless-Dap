idf_component_register(
    SRCS "swd_phy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_rom wdap_common
)
