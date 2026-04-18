idf_component_register(
    SRCS "ota_service.c"
    INCLUDE_DIRS "include"
    REQUIRES wdap_http_ota wifi_link
)
