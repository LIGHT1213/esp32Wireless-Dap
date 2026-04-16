idf_component_register(
    SRCS "cmsis_dap_usb.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos session_mgr wdap_common
)
