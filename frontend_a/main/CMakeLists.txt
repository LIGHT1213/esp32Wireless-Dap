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
)
