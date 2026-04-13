idf_component_register(
    SRCS "app_main.c"
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        wifi_link
        dap_backend
        swd_engine
        board_support
)
