idf_component_register(
    SRCS "app_main.c"
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        host_link
        wifi_link
        session_mgr
        dap_frontend
        cmsis_dap_usb
        usb_uart_bridge
        ota_service
        wdap_runtime
        wdap_common
)
