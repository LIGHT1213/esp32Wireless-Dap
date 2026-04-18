#include "ota_service.h"

#include "esp_check.h"
#include "esp_log.h"
#include "wdap_http_ota.h"
#include "wifi_link.h"

static const char *TAG = "ota_service_a";

esp_err_t ota_service_init(void)
{
    static const wdap_http_ota_config_t config = {
        .get_ip = wifi_link_get_local_ip_string,
        .index_html = NULL,
        .build_devices_json = NULL,
    };

    ESP_LOGI(TAG, "starting frontend OTA HTTP service");
    return wdap_http_ota_start(&config);
}
