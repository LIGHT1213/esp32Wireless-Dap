#include "dap_frontend.h"
#include "cmsis_dap_usb.h"
#include "host_link.h"
#include "session_mgr.h"
#include "wifi_link.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

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

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_link_init(session_mgr_handle_incoming, NULL));
    ESP_ERROR_CHECK(session_mgr_start());
    ESP_ERROR_CHECK(dap_frontend_init());
    ESP_ERROR_CHECK(cmsis_dap_usb_init());
    ESP_ERROR_CHECK(host_link_start(dap_frontend_handle_host_line, NULL));

    ESP_LOGI(TAG, "frontend A ready, commands are accepted via console and native USB CMSIS-DAP");
}
