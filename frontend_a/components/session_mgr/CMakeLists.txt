idf_component_register(
    SRCS "session_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos wdap_common wifi_link
)
