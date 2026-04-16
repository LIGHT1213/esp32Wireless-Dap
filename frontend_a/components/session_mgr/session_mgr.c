#include "session_mgr.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_utils.h"
#include "sdkconfig.h"
#include "transport_proto.h"
#include "wifi_link.h"

static const char *TAG = "session_mgr";
static const EventBits_t RESPONSE_READY_BIT = BIT0;

typedef struct {
    SemaphoreHandle_t tx_lock;
    SemaphoreHandle_t state_lock;
    EventGroupHandle_t event_group;
    uint16_t next_seq;
    uint16_t pending_seq;
    uint32_t last_activity_ms;
    bool pending;
    wdap_message_t response;
} session_mgr_state_t;

static session_mgr_state_t s_state;

static void __attribute__((unused)) heartbeat_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_WDAP_HEARTBEAT_INTERVAL_MS));

        if (!wifi_link_is_ready()) {
            continue;
        }

        bool pending = false;
        uint32_t idle_ms = 0;

        if (xSemaphoreTake(s_state.state_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            pending = s_state.pending;
            idle_ms = log_utils_uptime_ms() - s_state.last_activity_ms;
            xSemaphoreGive(s_state.state_lock);
        }

        if (pending || idle_ms < CONFIG_WDAP_HEARTBEAT_INTERVAL_MS) {
            continue;
        }

        wdap_ping_request_t ping_req = {
            .nonce = log_utils_uptime_ms(),
        };
        wdap_message_t response = {0};
        const esp_err_t err = session_mgr_send_command(WDAP_CMD_PING,
                                                       &ping_req,
                                                       sizeof(ping_req),
                                                       &response,
                                                       CONFIG_WDAP_REQUEST_TIMEOUT_MS);
        if (err != ESP_OK || response.status != WDAP_STATUS_OK) {
            ESP_LOGW(TAG, "heartbeat failed: err=%s status=%s",
                     esp_err_to_name(err),
                     wdap_status_to_string(response.status));
        }
    }
}

esp_err_t session_mgr_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.tx_lock = xSemaphoreCreateMutex();
    s_state.state_lock = xSemaphoreCreateMutex();
    s_state.event_group = xEventGroupCreate();
    s_state.next_seq = 1;
    s_state.last_activity_ms = log_utils_uptime_ms();

    if (s_state.tx_lock == NULL || s_state.state_lock == NULL || s_state.event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t session_mgr_start(void)
{
    ESP_LOGI(TAG, "heartbeat task disabled during hardware bring-up");
    return ESP_OK;
}

bool session_mgr_is_ready(void)
{
    return wifi_link_is_ready();
}

esp_err_t session_mgr_send_command(uint8_t cmd,
                                   const void *payload,
                                   uint16_t payload_len,
                                   wdap_message_t *response,
                                   uint32_t timeout_ms)
{
    if (payload_len > WDAP_MAX_PAYLOAD || response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_link_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_state.tx_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_state.state_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        xSemaphoreGive(s_state.tx_lock);
        return ESP_ERR_TIMEOUT;
    }

    wdap_message_t request = {
        .msg_type = WDAP_MSG_REQUEST,
        .cmd = cmd,
        .status = WDAP_STATUS_OK,
        .ack = WDAP_ACK_NONE,
        .seq = s_state.next_seq++,
        .payload_len = payload_len,
    };
    if (payload_len > 0U && payload != NULL) {
        memcpy(request.payload, payload, payload_len);
    }

    s_state.pending = true;
    s_state.pending_seq = request.seq;
    s_state.last_activity_ms = log_utils_uptime_ms();
    xEventGroupClearBits(s_state.event_group, RESPONSE_READY_BIT);
    xSemaphoreGive(s_state.state_lock);

    uint8_t encoded[WDAP_MAX_FRAME_SIZE];
    size_t encoded_size = 0;
    esp_err_t err = transport_proto_encode(&request, encoded, sizeof(encoded), &encoded_size);
    if (err != ESP_OK) {
        if (xSemaphoreTake(s_state.state_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_state.pending = false;
            xSemaphoreGive(s_state.state_lock);
        }
        xSemaphoreGive(s_state.tx_lock);
        return err;
    }

    const TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);

    for (int attempt = 0; attempt <= CONFIG_WDAP_FRONTEND_RETRY_COUNT; ++attempt) {
        err = wifi_link_send_packet(encoded, encoded_size);
        if (err != ESP_OK) {
            break;
        }

        const EventBits_t bits = xEventGroupWaitBits(s_state.event_group,
                                                     RESPONSE_READY_BIT,
                                                     pdTRUE,
                                                     pdFALSE,
                                                     wait_ticks);
        if ((bits & RESPONSE_READY_BIT) != 0) {
            if (xSemaphoreTake(s_state.state_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                *response = s_state.response;
                s_state.pending = false;
                s_state.last_activity_ms = log_utils_uptime_ms();
                xSemaphoreGive(s_state.state_lock);
                xSemaphoreGive(s_state.tx_lock);
                return ESP_OK;
            }

            err = ESP_ERR_TIMEOUT;
            break;
        }

        ESP_LOGW(TAG, "timeout cmd=%s seq=%u attempt=%d",
                 wdap_cmd_to_string(cmd),
                 request.seq,
                 attempt + 1);
        err = ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_state.state_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_state.pending = false;
        xSemaphoreGive(s_state.state_lock);
    }
    xSemaphoreGive(s_state.tx_lock);
    return err;
}

void session_mgr_handle_incoming(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;

    wdap_message_t message;
    const esp_err_t err = transport_proto_decode(data, len, &message);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "drop invalid frame: %s", esp_err_to_name(err));
        return;
    }

    if (xSemaphoreTake(s_state.state_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "drop frame because session lock is busy");
        return;
    }

    s_state.last_activity_ms = log_utils_uptime_ms();

    if (!s_state.pending || message.seq != s_state.pending_seq) {
        ESP_LOGW(TAG, "unexpected response seq=%u pending=%u cmd=%s",
                 message.seq,
                 s_state.pending_seq,
                 wdap_cmd_to_string(message.cmd));
        xSemaphoreGive(s_state.state_lock);
        return;
    }

    s_state.response = message;
    xEventGroupSetBits(s_state.event_group, RESPONSE_READY_BIT);
    xSemaphoreGive(s_state.state_lock);
}
