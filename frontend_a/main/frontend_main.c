#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_dap_frontend.h"
#include "wdap_transport.h"

static const char *TAG = "frontend_a";

static void run_ping_self_test(void)
{
    const uint8_t ping[] = {'P', 'I', 'N', 'G'};
    wdap_message_t message;
    for (int attempt = 0; attempt < 3; ++attempt) {
        esp_err_t err = wdap_transport_send_message(WDAP_PKT_PING, ping, sizeof(ping), WDAP_TRANSFER_TIMEOUT_MS);
        if (err == ESP_OK) {
            err = wdap_transport_recv_message(&message, WDAP_TRANSFER_TIMEOUT_MS);
            if (err == ESP_OK && message.type == WDAP_PKT_PONG) {
                ESP_LOGI(TAG, "PING/PONG ok; DAP queue now dedicated to Keil");
                wdap_transport_log_stats();
                return;
            }
            ESP_LOGW(TAG, "PING/PONG missing err=%s type=%u", esp_err_to_name(err), err == ESP_OK ? (unsigned)message.type : 0U);
        } else {
            ESP_LOGW(TAG, "PING send failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    wdap_transport_log_stats();
}

void app_main(void)
{
    ESP_ERROR_CHECK(wdap_transport_init());
    run_ping_self_test();
    ESP_ERROR_CHECK(usb_dap_frontend_init());
    ESP_LOGI(TAG, "frontend_a ready");
}
