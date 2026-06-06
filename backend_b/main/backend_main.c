#include <string.h>
#include "daplink_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wdap_transport.h"

static const char *TAG = "backend_b";
#define WDAP_BACKEND_DAP_TRACE_LIMIT 0U
#if WDAP_BACKEND_DAP_TRACE_LIMIT > 0U
static uint32_t s_dap_trace_count;
#endif

static void backend_task(void *arg)
{
    (void)arg;
    wdap_message_t message;
    uint8_t response[WDAP_DAP_PACKET_SIZE];

    while (1) {
        esp_err_t err = wdap_transport_recv_message(&message, UINT32_MAX);
        if (err != ESP_OK) {
            continue;
        }

        if (message.type == WDAP_PKT_PING) {
            const uint8_t pong[] = {'P', 'O', 'N', 'G'};
            ESP_LOGI(TAG, "PING received, sending PONG");
            ESP_ERROR_CHECK_WITHOUT_ABORT(wdap_transport_send_message(WDAP_PKT_PONG, pong, sizeof(pong), WDAP_TRANSFER_TIMEOUT_MS));
            continue;
        }

        if (message.type == WDAP_PKT_DAP_BATCH_NORESP) {
            if (message.len == 0) {
                ESP_LOGW(TAG, "empty DAP batch");
                continue;
            }
            uint8_t count = message.payload[0];
            size_t offset = 1U;
            for (uint8_t index = 0; index < count; ++index) {
                if (offset >= message.len) {
                    ESP_LOGW(TAG, "truncated DAP batch index=%u len=%u", (unsigned)index, (unsigned)message.len);
                    break;
                }
                size_t dap_len = message.payload[offset++];
                if (dap_len == 0U || offset + dap_len > message.len) {
                    ESP_LOGW(TAG, "invalid DAP batch entry index=%u dap_len=%u total=%u", (unsigned)index, (unsigned)dap_len, (unsigned)message.len);
                    break;
                }
                memset(response, 0, sizeof(response));
                size_t response_len = daplink_port_process(message.payload + offset, dap_len, response, sizeof(response));
#if WDAP_BACKEND_DAP_TRACE_LIMIT > 0U
                if (s_dap_trace_count < WDAP_BACKEND_DAP_TRACE_LIMIT) {
                    ESP_LOGI(TAG, "DAP batch[%u/%u] len=%u cmd=0x%02x b1=%02x b2=%02x b3=%02x rsp_len=%u r0=%02x r1=%02x r2=%02x r3=%02x",
                             (unsigned)(index + 1U), (unsigned)count, (unsigned)dap_len, message.payload[offset],
                             dap_len > 1U ? message.payload[offset + 1U] : 0U,
                             dap_len > 2U ? message.payload[offset + 2U] : 0U,
                             dap_len > 3U ? message.payload[offset + 3U] : 0U,
                             (unsigned)response_len, response[0], response[1], response[2], response[3]);
                    s_dap_trace_count++;
                }
#else
                (void)response_len;
#endif
                offset += dap_len;
            }
            continue;
        }

        bool no_response = (message.type == WDAP_PKT_DAP_REQ_NORESP);
        if (message.type != WDAP_PKT_DAP_REQ && !no_response) {
            ESP_LOGW(TAG, "unexpected message type=%u len=%u", (unsigned)message.type, (unsigned)message.len);
            continue;
        }

        memset(response, 0, sizeof(response));
        size_t response_len = daplink_port_process(message.payload, message.len, response, sizeof(response));
#if WDAP_BACKEND_DAP_TRACE_LIMIT > 0U
        if (s_dap_trace_count < WDAP_BACKEND_DAP_TRACE_LIMIT) {
            ESP_LOGI(TAG, "DAP type=%u len=%u cmd=0x%02x b1=%02x b2=%02x b3=%02x rsp_len=%u r0=%02x r1=%02x r2=%02x r3=%02x",
                     (unsigned)message.type, (unsigned)message.len, message.payload[0],
                     message.len > 1U ? message.payload[1U] : 0U,
                     message.len > 2U ? message.payload[2U] : 0U,
                     message.len > 3U ? message.payload[3U] : 0U,
                     (unsigned)response_len, response[0], response[1], response[2], response[3]);
            s_dap_trace_count++;
        }
#endif
        if (no_response) {
            continue;
        }
        if (response_len == 0) {
            response[0] = 0xff;
            response_len = 1;
        }
        err = wdap_transport_send_message(WDAP_PKT_DAP_RSP, response, response_len, WDAP_TRANSFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "DAP response send failed: %s", esp_err_to_name(err));
        }
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    while (1) {
        wdap_transport_log_stats();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(wdap_transport_init());
    ESP_ERROR_CHECK(daplink_port_init());
    xTaskCreatePinnedToCore(backend_task, "backend_task", 8192, NULL, 21, NULL, WDAP_DAP_TASK_CORE_ID);
    xTaskCreate(stats_task, "stats_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "backend_b ready");
}
