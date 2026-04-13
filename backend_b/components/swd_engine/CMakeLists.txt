idf_component_register(
    SRCS "swd_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES board_support swd_phy wdap_common
)
