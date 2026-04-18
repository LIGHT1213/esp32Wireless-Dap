#include "dap_frontend.h"
#include "cmsis_dap_usb.h"
#include "host_link.h"
#include "session_mgr.h"
#include "transport_proto.h"
#include "usb_uart_bridge.h"
#include "wifi_link.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ota_service.h"
#include "wdap_runtime.h"

static const char *TAG = "frontend_main";

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void frontend_wifi_rx_router(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;

    wdap_message_t message = {0};
    if (transport_proto_decode(data, len, &message) != ESP_OK) {
        ESP_LOGW(TAG, "drop undecodable frame from backend");
        return;
    }

    switch (message.msg_type) {
    case WDAP_MSG_RESPONSE:
        session_mgr_handle_incoming(data, len, NULL);
        return;
    case WDAP_MSG_STREAM:
        if (usb_uart_bridge_handle_message(&message) != ESP_OK) {
            ESP_LOGW(TAG, "drop stream cmd=%s", wdap_cmd_to_string(message.cmd));
        }
        return;
    default:
        ESP_LOGW(TAG, "ignore unexpected msg_type=0x%02x cmd=%s", message.msg_type, wdap_cmd_to_string(message.cmd));
        return;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(wdap_runtime_init("frontend_a"));
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(wdap_runtime_mark_running_partition_valid());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG
    esp_log_level_set("cmsis_dap_usb", ESP_LOG_DEBUG);
    esp_log_level_set("session_mgr", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_link_a", ESP_LOG_DEBUG);
#endif

    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_link_init(frontend_wifi_rx_router, NULL));
    ESP_ERROR_CHECK(session_mgr_start());
    ESP_ERROR_CHECK(dap_frontend_init());
    ESP_ERROR_CHECK(cmsis_dap_usb_init());
    ESP_ERROR_CHECK(usb_uart_bridge_init());
    ESP_ERROR_CHECK(ota_service_init());
    ESP_ERROR_CHECK(host_link_start(dap_frontend_handle_host_line, NULL));

    ESP_LOGI(TAG, "frontend A ready, commands are accepted via console, native USB CMSIS-DAP, USB CDC bridge, and HTTP OTA");
}
