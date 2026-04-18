#include "board_support.h"
#include "swd_engine.h"
#include "wifi_link.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "backend_main";

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

#if CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG
    esp_log_level_set("dap_backend", ESP_LOG_DEBUG);
    esp_log_level_set("swd_engine", ESP_LOG_DEBUG);
    esp_log_level_set("swd_phy", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_link_b", ESP_LOG_DEBUG);
#endif

    ESP_ERROR_CHECK(board_support_init());
    ESP_ERROR_CHECK(swd_engine_init());
    ESP_ERROR_CHECK(wifi_link_init());

    ESP_LOGI(TAG, "backend B ready, waiting for frontend requests");
}
