set(DAPLINK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/DAPLink")

idf_component_register(
    SRCS
        "src/daplink_port.c"
        "${DAPLINK_ROOT}/source/daplink/cmsis-dap/DAP.c"
        "${DAPLINK_ROOT}/source/daplink/cmsis-dap/SW_DP.c"
    INCLUDE_DIRS
        "include"
        "${DAPLINK_ROOT}/source/daplink/cmsis-dap"
        "${DAPLINK_ROOT}/source/daplink"
    REQUIRES backend_swd_gpio board_config esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE __CC_ARM=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-error=unknown-pragmas)
