#include "wdap_transport.h"

#include <stdlib.h>
#include <string.h>
#include "board_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

typedef enum {
    WDAP_EVT_RECV,
    WDAP_EVT_SEND,
} wdap_evt_id_t;

typedef struct {
    wdap_evt_id_t id;
    uint8_t mac[6];
    esp_now_send_status_t status;
    size_t len;
    uint8_t data[sizeof(wdap_hdr_t) + WDAP_ESPNOW_PAYLOAD_MTU];
} wdap_evt_t;

typedef struct {
    uint32_t msg_id;
    uint16_t frag_cnt;
    uint16_t next_frag;
    wdap_pkt_type_t type;
    size_t len;
    uint8_t data[WDAP_MAX_MESSAGE_SIZE];
    bool active;
} wdap_reassembly_t;

static const char *TAG = "wdap_transport";
static QueueHandle_t s_evt_queue;
static QueueHandle_t s_msg_queue;
static SemaphoreHandle_t s_ack_sem;
static SemaphoreHandle_t s_send_sem;
static uint8_t s_peer_mac[6];
static uint16_t s_tx_seq;
static uint32_t s_tx_msg_id = 1;
static volatile uint16_t s_wait_ack_seq;
static volatile esp_now_send_status_t s_last_send_status;
static volatile bool s_last_ack_is_nack;
static volatile bool s_send_status_needed;
static wdap_reassembly_t s_reassembly;
static wdap_transport_stats_t s_stats;

static bool use_app_ack(wdap_pkt_type_t type, uint16_t frag_cnt)
{
    return frag_cnt > 1U || (type != WDAP_PKT_DAP_REQ && type != WDAP_PKT_DAP_RSP && type != WDAP_PKT_DAP_REQ_NORESP && type != WDAP_PKT_DAP_BATCH_NORESP);
}

static void wdap_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (s_evt_queue == NULL || mac_addr == NULL || !s_send_status_needed) {
        return;
    }
    wdap_evt_t evt = {.id = WDAP_EVT_SEND, .status = status};
    memcpy(evt.mac, mac_addr, sizeof(evt.mac));
    xQueueSend(s_evt_queue, &evt, 0);
}

static void wdap_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (s_evt_queue == NULL || recv_info == NULL || data == NULL || len <= 0 || len > (int)sizeof(((wdap_evt_t *)0)->data)) {
        return;
    }
    wdap_evt_t evt = {.id = WDAP_EVT_RECV, .len = (size_t)len};
    memcpy(evt.mac, recv_info->src_addr, sizeof(evt.mac));
    memcpy(evt.data, data, evt.len);
    xQueueSend(s_evt_queue, &evt, 0);
}

static esp_err_t send_frame(wdap_pkt_type_t type, uint32_t msg_id, uint16_t seq, uint16_t ack_seq,
                            uint16_t frag_idx, uint16_t frag_cnt, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[sizeof(wdap_hdr_t) + WDAP_ESPNOW_PAYLOAD_MTU];
    wdap_hdr_t hdr = {
        .magic = WDAP_MAGIC,
        .version = WDAP_VERSION,
        .type = (uint8_t)type,
        .header_len = sizeof(wdap_hdr_t),
        .session_id = WDAP_SESSION_ID,
        .msg_id = msg_id,
        .seq = seq,
        .ack_seq = ack_seq,
        .frag_idx = frag_idx,
        .frag_cnt = frag_cnt,
        .payload_len = payload_len,
        .flags = 0,
        .crc32 = 0,
    };
    hdr.crc32 = wdap_protocol_crc32(&hdr, payload);
    memcpy(frame, &hdr, sizeof(hdr));
    if (payload_len > 0 && payload != NULL) {
        memcpy(frame + sizeof(hdr), payload, payload_len);
    }

    esp_err_t err = esp_now_send(s_peer_mac, frame, sizeof(hdr) + payload_len);
    if (err == ESP_OK) {
        s_stats.tx_frames++;
    } else {
        s_stats.send_failures++;
    }
    return err;
}

static void send_ack(uint16_t seq, bool nack)
{
    esp_err_t err = send_frame(nack ? WDAP_PKT_NACK : WDAP_PKT_ACK, 0, 0, seq, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        if (nack) {
            s_stats.nack_tx++;
        } else {
            s_stats.ack_tx++;
        }
    }
}

static void process_complete_message(const wdap_hdr_t *hdr, const uint8_t *payload)
{
    wdap_message_t message = {
        .type = (wdap_pkt_type_t)hdr->type,
        .msg_id = hdr->msg_id,
        .len = hdr->payload_len,
    };
    if (message.len > sizeof(message.payload)) {
        s_stats.reassembly_errors++;
        send_ack(hdr->seq, true);
        return;
    }
    if (message.len > 0) {
        memcpy(message.payload, payload, message.len);
    }
    bool ack_enabled = use_app_ack((wdap_pkt_type_t)hdr->type, hdr->frag_cnt);
    if (xQueueSend(s_msg_queue, &message, 0) == pdTRUE) {
        s_stats.rx_messages++;
        if (ack_enabled) {
            send_ack(hdr->seq, false);
        }
    } else {
        s_stats.reassembly_errors++;
        if (ack_enabled) {
            send_ack(hdr->seq, true);
        }
    }
}

static void process_fragment(const wdap_hdr_t *hdr, const uint8_t *payload)
{
    if (hdr->frag_cnt <= 1) {
        process_complete_message(hdr, payload);
        return;
    }

    if (hdr->frag_idx == 0) {
        s_reassembly.active = true;
        s_reassembly.msg_id = hdr->msg_id;
        s_reassembly.frag_cnt = hdr->frag_cnt;
        s_reassembly.next_frag = 0;
        s_reassembly.type = (wdap_pkt_type_t)hdr->type;
        s_reassembly.len = 0;
    }

    if (!s_reassembly.active || s_reassembly.msg_id != hdr->msg_id || s_reassembly.next_frag != hdr->frag_idx) {
        s_stats.reassembly_errors++;
        send_ack(hdr->seq, true);
        return;
    }

    if (s_reassembly.len + hdr->payload_len > sizeof(s_reassembly.data)) {
        s_reassembly.active = false;
        s_stats.reassembly_errors++;
        send_ack(hdr->seq, true);
        return;
    }

    memcpy(s_reassembly.data + s_reassembly.len, payload, hdr->payload_len);
    s_reassembly.len += hdr->payload_len;
    s_reassembly.next_frag++;
    send_ack(hdr->seq, false);

    if (s_reassembly.next_frag == s_reassembly.frag_cnt) {
        wdap_message_t message = {
            .type = s_reassembly.type,
            .msg_id = s_reassembly.msg_id,
            .len = s_reassembly.len,
        };
        memcpy(message.payload, s_reassembly.data, message.len);
        if (xQueueSend(s_msg_queue, &message, 0) == pdTRUE) {
            s_stats.rx_messages++;
        } else {
            s_stats.reassembly_errors++;
        }
        s_reassembly.active = false;
    }
}

static void transport_task(void *arg)
{
    (void)arg;
    wdap_evt_t evt;
    while (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
        if (evt.id == WDAP_EVT_SEND) {
            s_last_send_status = evt.status;
            xSemaphoreGive(s_send_sem);
            continue;
        }

        const wdap_hdr_t *hdr = (const wdap_hdr_t *)evt.data;
        const uint8_t *payload = evt.data + sizeof(wdap_hdr_t);
        if (!wdap_protocol_validate(hdr, payload, evt.len)) {
            s_stats.crc_errors++;
            continue;
        }
        s_stats.rx_frames++;

        if (hdr->type == WDAP_PKT_ACK || hdr->type == WDAP_PKT_NACK) {
            if (hdr->ack_seq == s_wait_ack_seq) {
                if (hdr->type == WDAP_PKT_ACK) {
                    s_stats.ack_rx++;
                    s_last_ack_is_nack = false;
                } else {
                    s_stats.nack_rx++;
                    s_last_ack_is_nack = true;
                }
                xSemaphoreGive(s_ack_sem);
            }
            continue;
        }

        process_fragment(hdr, payload);
    }
}

esp_err_t wdap_transport_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs_flash_init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "esp_wifi_set_ps failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N), TAG, "esp_wifi_set_protocol failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20), TAG, "esp_wifi_set_bandwidth failed");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_max_tx_power(78));
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(WDAP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG, "esp_wifi_set_channel failed");

    uint8_t sta_mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, sta_mac), TAG, "esp_wifi_get_mac failed");
    wdap_board_get_peer_mac(s_peer_mac);
    char sta_str[18];
    char peer_str[18];
    wdap_protocol_format_mac(sta_mac, sta_str);
    wdap_protocol_format_mac(s_peer_mac, peer_str);
    ESP_LOGI(TAG, "%s Wi-Fi STA init ok, sta_mac=%s peer_mac=%s channel=%d", wdap_board_role_name(), sta_str, peer_str, WDAP_ESPNOW_CHANNEL);

    s_evt_queue = xQueueCreate(64, sizeof(wdap_evt_t));
    s_msg_queue = xQueueCreate(64, sizeof(wdap_message_t));
    s_ack_sem = xSemaphoreCreateBinary();
    s_send_sem = xSemaphoreCreateBinary();
    if (s_evt_queue == NULL || s_msg_queue == NULL || s_ack_sem == NULL || s_send_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(transport_task, "wdap_transport", 6144, NULL, 22, NULL, WDAP_TRANSPORT_TASK_CORE_ID);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp_now_init failed");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(wdap_send_cb), TAG, "esp_now_register_send_cb failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(wdap_recv_cb), TAG, "esp_now_register_recv_cb failed");

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_peer_mac, sizeof(peer.peer_addr));
    peer.channel = WDAP_ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK && ret != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer failed peer=%s channel=%d err=0x%x", peer_str, WDAP_ESPNOW_CHANNEL, ret);
        return ret;
    }
    esp_now_rate_config_t rate_config = {
        .phymode = WIFI_PHY_MODE_HT20,
        .rate = WIFI_PHY_RATE_MCS7_SGI,
        .ersu = false,
        .dcm = false,
    };
    ret = esp_now_set_peer_rate_config(s_peer_mac, &rate_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_set_peer_rate_config failed peer=%s err=0x%x; using default rate", peer_str, ret);
    }

    ESP_LOGI(TAG, "esp_now_add_peer ok peer=%s channel=%d mtu=%u rate=%s", peer_str, WDAP_ESPNOW_CHANNEL,
             (unsigned)WDAP_ESPNOW_PAYLOAD_MTU, ret == ESP_OK ? "MCS7_SGI" : "default");
    return ESP_OK;
}

esp_err_t wdap_transport_send_message(wdap_pkt_type_t type, const uint8_t *payload, size_t len, uint32_t timeout_ms)
{
    if (len > WDAP_MAX_MESSAGE_SIZE || (len > 0 && payload == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t msg_id = s_tx_msg_id++;
    uint16_t frag_cnt = (uint16_t)((len + WDAP_ESPNOW_PAYLOAD_MTU - 1) / WDAP_ESPNOW_PAYLOAD_MTU);
    if (frag_cnt == 0) {
        frag_cnt = 1;
    }

    for (uint16_t frag_idx = 0; frag_idx < frag_cnt; ++frag_idx) {
        size_t offset = (size_t)frag_idx * WDAP_ESPNOW_PAYLOAD_MTU;
        size_t remain = len > offset ? len - offset : 0;
        uint16_t frag_len = (uint16_t)(remain > WDAP_ESPNOW_PAYLOAD_MTU ? WDAP_ESPNOW_PAYLOAD_MTU : remain);
        const uint8_t *frag_payload = frag_len > 0 ? payload + offset : NULL;
        uint16_t seq = ++s_tx_seq;
        s_wait_ack_seq = seq;

        bool ack_enabled = use_app_ack(type, frag_cnt);
        for (int attempt = 0; attempt <= WDAP_ESPNOW_RETRY_COUNT; ++attempt) {
            while (xSemaphoreTake(s_ack_sem, 0) == pdTRUE) {}
            while (xSemaphoreTake(s_send_sem, 0) == pdTRUE) {}
            s_last_ack_is_nack = false;
            s_send_status_needed = ack_enabled;
            esp_err_t send_err = send_frame(type, msg_id, seq, 0, frag_idx, frag_cnt, frag_payload, frag_len);
            if (send_err != ESP_OK) {
                s_send_status_needed = false;
                ESP_RETURN_ON_ERROR(send_err, TAG, "esp_now_send failed");
            }
            if (!ack_enabled) {
                s_send_status_needed = false;
                break;
            }
            if (xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE || s_last_send_status != ESP_NOW_SEND_SUCCESS) {
                s_send_status_needed = false;
                s_stats.timeouts++;
                s_stats.retries++;
                continue;
            }
            s_send_status_needed = false;
            if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE && !s_last_ack_is_nack) {
                break;
            }
            s_stats.timeouts++;
            s_stats.retries++;
            if (attempt == WDAP_ESPNOW_RETRY_COUNT) {
                ESP_LOGE(TAG, "message timeout type=%u msg=%lu frag=%u/%u seq=%u", (unsigned)type, (unsigned long)msg_id,
                         frag_idx + 1, frag_cnt, seq);
                return ESP_ERR_TIMEOUT;
            }
        }
    }

    s_stats.tx_messages++;
    return ESP_OK;
}

esp_err_t wdap_transport_recv_message(wdap_message_t *message, uint32_t timeout_ms)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_msg_queue, message, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void wdap_transport_get_stats(wdap_transport_stats_t *stats)
{
    if (stats != NULL) {
        *stats = s_stats;
    }
}

void wdap_transport_log_stats(void)
{
    ESP_LOGI(TAG, "stats tx_frames=%lu rx_frames=%lu tx_msg=%lu rx_msg=%lu ack_rx=%lu ack_tx=%lu nack_rx=%lu nack_tx=%lu retries=%lu timeouts=%lu crc=%lu dup=%lu reasm=%lu send_fail=%lu",
             (unsigned long)s_stats.tx_frames, (unsigned long)s_stats.rx_frames,
             (unsigned long)s_stats.tx_messages, (unsigned long)s_stats.rx_messages,
             (unsigned long)s_stats.ack_rx, (unsigned long)s_stats.ack_tx,
             (unsigned long)s_stats.nack_rx, (unsigned long)s_stats.nack_tx,
             (unsigned long)s_stats.retries, (unsigned long)s_stats.timeouts,
             (unsigned long)s_stats.crc_errors, (unsigned long)s_stats.duplicates,
             (unsigned long)s_stats.reassembly_errors, (unsigned long)s_stats.send_failures);
}
