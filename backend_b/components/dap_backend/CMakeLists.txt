idf_component_register(
    SRCS "dap_backend.c"
    INCLUDE_DIRS "include"
    REQUIRES wdap_common swd_engine wdap_runtime
)
