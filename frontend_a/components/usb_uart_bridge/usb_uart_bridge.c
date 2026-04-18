#include "usb_uart_bridge.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb_cdc_acm.h"
#include "transport_proto.h"
#include "wdap_runtime.h"
#include "wifi_link.h"

static const char *TAG = "usb_uart_bridge";

#define USB_UART_BRIDGE_CDC_PORT TINYUSB_CDC_ACM_0
#define USB_UART_BRIDGE_MAX_CHUNK 256U
#define USB_UART_BRIDGE_PEER_QUEUE_LEN 16U
#define USB_UART_BRIDGE_HOST_QUEUE_LEN 16U
#define USB_UART_BRIDGE_TASK_STACK_SIZE 4096U
#define USB_UART_BRIDGE_USB_CORE_ID 1
#define USB_UART_BRIDGE_NET_CORE_ID 0
#define USB_UART_BRIDGE_HOST_TX_STALL_MS 1000U

typedef enum {
    USB_UART_BRIDGE_PEER_EVT_DATA = 0,
    USB_UART_BRIDGE_PEER_EVT_CONFIG = 1,
} usb_uart_bridge_peer_evt_type_t;

typedef struct {
    uint16_t len;
    uint8_t data[USB_UART_BRIDGE_MAX_CHUNK];
} usb_uart_bridge_host_evt_t;

typedef struct {
    uint8_t type;
    uint16_t len;
    wdap_uart_config_t config;
    uint8_t data[USB_UART_BRIDGE_MAX_CHUNK];
} usb_uart_bridge_peer_evt_t;

typedef struct {
    QueueHandle_t peer_tx_queue;
    QueueHandle_t host_tx_queue;
    TaskHandle_t peer_tx_task;
    TaskHandle_t host_tx_task;
    wdap_uart_config_t current_config;
    uint16_t next_seq;
    bool initialized;
} usb_uart_bridge_state_t;

static usb_uart_bridge_state_t s_state = {
    .current_config = {
        .bit_rate = 115200,
        .stop_bits = 0,
        .parity = 0,
        .data_bits = 8,
        .dtr = 0,
        .rts = 0,
        .reserved = {0, 0},
    },
    .next_seq = 1,
};

static portMUX_TYPE s_config_lock = portMUX_INITIALIZER_UNLOCKED;

static uint16_t next_stream_seq(void)
{
    return s_state.next_seq++;
}

static esp_err_t encode_stream_message(uint8_t cmd,
                                       const void *payload,
                                       uint16_t payload_len,
                                       uint8_t *encoded,
                                       size_t encoded_capacity,
                                       size_t *encoded_size)
{
    wdap_message_t message = {
        .msg_type = WDAP_MSG_STREAM,
        .cmd = cmd,
        .status = WDAP_STATUS_OK,
        .ack = WDAP_ACK_NONE,
        .seq = next_stream_seq(),
        .payload_len = payload_len,
    };
    if (payload_len > 0U && payload != NULL) {
        memcpy(message.payload, payload, payload_len);
    }
    return transport_proto_encode(&message, encoded, encoded_capacity, encoded_size);
}

static void queue_config_to_peer(void)
{
    if (s_state.peer_tx_queue == NULL) {
        return;
    }
    if (wdap_runtime_is_busy()) {
        ESP_LOGW(TAG, "drop USB CDC RX because OTA is active");
        return;
    }

    usb_uart_bridge_peer_evt_t event = {
        .type = USB_UART_BRIDGE_PEER_EVT_CONFIG,
        .len = sizeof(event.config),
    };

    taskENTER_CRITICAL(&s_config_lock);
    event.config = s_state.current_config;
    taskEXIT_CRITICAL(&s_config_lock);

    if (xQueueSend(s_state.peer_tx_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop UART config update because peer queue is full");
    }
}

static void usb_uart_bridge_line_state_changed_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;

    taskENTER_CRITICAL(&s_config_lock);
    s_state.current_config.dtr = event->line_state_changed_data.dtr ? 1U : 0U;
    s_state.current_config.rts = event->line_state_changed_data.rts ? 1U : 0U;
    taskEXIT_CRITICAL(&s_config_lock);

    queue_config_to_peer();
}

static void usb_uart_bridge_line_coding_changed_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;

    const cdc_line_coding_t *line_coding = event->line_coding_changed_data.p_line_coding;
    taskENTER_CRITICAL(&s_config_lock);
    s_state.current_config.bit_rate = line_coding->bit_rate;
    s_state.current_config.stop_bits = line_coding->stop_bits;
    s_state.current_config.parity = line_coding->parity;
    s_state.current_config.data_bits = line_coding->data_bits;
    taskEXIT_CRITICAL(&s_config_lock);

    queue_config_to_peer();
}

static void usb_uart_bridge_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    (void)event;

    if (s_state.peer_tx_queue == NULL) {
        return;
    }

    while (true) {
        usb_uart_bridge_peer_evt_t peer_event = {
            .type = USB_UART_BRIDGE_PEER_EVT_DATA,
        };
        size_t rx_size = 0;
        const esp_err_t err = tinyusb_cdcacm_read(USB_UART_BRIDGE_CDC_PORT,
                                                  peer_event.data,
                                                  sizeof(peer_event.data),
                                                  &rx_size);
        if (err != ESP_OK || rx_size == 0U) {
            return;
        }

        peer_event.len = (uint16_t)rx_size;
        if (xQueueSend(s_state.peer_tx_queue, &peer_event, 0) != pdTRUE) {
            ESP_LOGW(TAG, "drop USB CDC data because peer queue is full");
            return;
        }
    }
}

static void usb_uart_bridge_peer_tx_task(void *arg)
{
    (void)arg;

    usb_uart_bridge_peer_evt_t event;
    uint8_t encoded[WDAP_MAX_FRAME_SIZE];

    while (true) {
        if (xQueueReceive(s_state.peer_tx_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        size_t encoded_size = 0;
        const void *payload = NULL;
        uint16_t payload_len = 0;
        uint8_t cmd = 0;

        if (event.type == USB_UART_BRIDGE_PEER_EVT_DATA) {
            payload = event.data;
            payload_len = event.len;
            cmd = WDAP_CMD_UART_DATA;
        } else if (event.type == USB_UART_BRIDGE_PEER_EVT_CONFIG) {
            payload = &event.config;
            payload_len = sizeof(event.config);
            cmd = WDAP_CMD_UART_CONFIG;
        } else {
            continue;
        }

        if (encode_stream_message(cmd, payload, payload_len, encoded, sizeof(encoded), &encoded_size) != ESP_OK) {
            ESP_LOGW(TAG, "encode stream cmd=%s failed", wdap_cmd_to_string(cmd));
            continue;
        }

        const esp_err_t err = wifi_link_send_packet(encoded, encoded_size);
        if (err == ESP_ERR_INVALID_STATE && event.type == USB_UART_BRIDGE_PEER_EVT_CONFIG) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (xQueueSendToFront(s_state.peer_tx_queue, &event, 0) != pdTRUE) {
                ESP_LOGW(TAG, "drop deferred UART config because peer queue is full");
            }
            continue;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send stream cmd=%s failed", wdap_cmd_to_string(cmd));
        }
    }
}

static void usb_uart_bridge_host_tx_task(void *arg)
{
    (void)arg;

    usb_uart_bridge_host_evt_t event;

    while (true) {
        if (xQueueReceive(s_state.host_tx_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        size_t offset = 0U;
        TickType_t stall_since = xTaskGetTickCount();
        while (offset < event.len) {
            const size_t written = tinyusb_cdcacm_write_queue(USB_UART_BRIDGE_CDC_PORT,
                                                              &event.data[offset],
                                                              event.len - offset);
            if (written == 0U) {
                const esp_err_t flush_err = tinyusb_cdcacm_write_flush(USB_UART_BRIDGE_CDC_PORT, pdMS_TO_TICKS(20));
                if (flush_err == ESP_ERR_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
                if ((xTaskGetTickCount() - stall_since) > pdMS_TO_TICKS(USB_UART_BRIDGE_HOST_TX_STALL_MS)) {
                    ESP_LOGW(TAG, "drop UART data from backend because CDC TX stalled");
                    break;
                }
                continue;
            }

            offset += written;
            stall_since = xTaskGetTickCount();
            (void)tinyusb_cdcacm_write_flush(USB_UART_BRIDGE_CDC_PORT, pdMS_TO_TICKS(20));
        }
    }
}

esp_err_t usb_uart_bridge_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }

    s_state.peer_tx_queue = xQueueCreate(USB_UART_BRIDGE_PEER_QUEUE_LEN, sizeof(usb_uart_bridge_peer_evt_t));
    s_state.host_tx_queue = xQueueCreate(USB_UART_BRIDGE_HOST_QUEUE_LEN, sizeof(usb_uart_bridge_host_evt_t));
    if (s_state.peer_tx_queue == NULL || s_state.host_tx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = USB_UART_BRIDGE_CDC_PORT,
        .callback_rx = usb_uart_bridge_rx_cb,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = usb_uart_bridge_line_state_changed_cb,
        .callback_line_coding_changed = usb_uart_bridge_line_coding_changed_cb,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&cdc_config), TAG, "cdc acm init failed");

    BaseType_t ok = xTaskCreatePinnedToCore(usb_uart_bridge_peer_tx_task,
                                            "uart_peer_tx",
                                            USB_UART_BRIDGE_TASK_STACK_SIZE,
                                            NULL,
                                            4,
                                            &s_state.peer_tx_task,
                                            USB_UART_BRIDGE_NET_CORE_ID);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ok = xTaskCreatePinnedToCore(usb_uart_bridge_host_tx_task,
                                 "uart_host_tx",
                                 USB_UART_BRIDGE_TASK_STACK_SIZE,
                                 NULL,
                                 4,
                                 &s_state.host_tx_task,
                                 USB_UART_BRIDGE_USB_CORE_ID);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "USB CDC UART bridge ready");
    return ESP_OK;
}

esp_err_t usb_uart_bridge_handle_message(const wdap_message_t *message)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (message->cmd) {
    case WDAP_CMD_UART_DATA:
        if (wdap_runtime_is_busy()) {
            return ESP_ERR_INVALID_STATE;
        }
        for (uint16_t offset = 0; offset < message->payload_len; offset += USB_UART_BRIDGE_MAX_CHUNK) {
            const uint16_t chunk_len = (uint16_t)((message->payload_len - offset) > USB_UART_BRIDGE_MAX_CHUNK
                                                      ? USB_UART_BRIDGE_MAX_CHUNK
                                                      : (message->payload_len - offset));
            usb_uart_bridge_host_evt_t event = {
                .len = chunk_len,
            };
            memcpy(event.data, &message->payload[offset], chunk_len);
            if (xQueueSend(s_state.host_tx_queue, &event, 0) != pdTRUE) {
                ESP_LOGW(TAG, "drop UART data from backend because host queue is full");
                return ESP_ERR_NO_MEM;
            }
        }
        return ESP_OK;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}
