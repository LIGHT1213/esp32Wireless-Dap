idf_component_register(
    SRCS "dap_frontend.c"
    INCLUDE_DIRS "include"
    REQUIRES session_mgr wdap_common
)
