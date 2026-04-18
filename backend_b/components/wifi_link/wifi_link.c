#include "wifi_link.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dap_backend.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "log_utils.h"
#include "sdkconfig.h"
#include "transport_proto.h"
#include "uart_bridge.h"
#include "wdap_protocol.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "wifi_link_b";
static const int WDAP_SOCKET_BUFFER_BYTES = (int)(WDAP_MAX_FRAME_SIZE * 8U);
static const BaseType_t WDAP_SWD_CORE_ID = 1;

static int s_socket = -1;
static SemaphoreHandle_t s_socket_lock;
static struct sockaddr_storage s_peer_addr;
static socklen_t s_peer_addr_len;
static bool s_peer_valid;
static esp_netif_t *s_wifi_netif;

typedef struct {
    bool present;
    uint32_t last_seen_ms;
    uint16_t http_port;
    char ip[16];
} frontend_peer_state_t;

static frontend_peer_state_t s_frontend_peer;
static const uint32_t WDAP_FRONTEND_ONLINE_TIMEOUT_MS = 15000U;

static void close_socket_locked(void)
{
    if (s_socket >= 0) {
        shutdown(s_socket, 0);
        close(s_socket);
        s_socket = -1;
    }
    s_peer_valid = false;
    s_peer_addr_len = 0;
    memset(&s_peer_addr, 0, sizeof(s_peer_addr));
}

static void update_frontend_peer_locked(const struct sockaddr_storage *peer_addr,
                                        socklen_t peer_len,
                                        const wdap_message_t *message)
{
    (void)peer_len;

    if (peer_addr == NULL || message == NULL) {
        return;
    }
    if (peer_addr->ss_family != AF_INET) {
        return;
    }
    if (message->payload_len < sizeof(wdap_device_announce_t)) {
        return;
    }

    const wdap_device_announce_t *announce = (const wdap_device_announce_t *)message->payload;
    if (announce->role != WDAP_DEVICE_ROLE_FRONTEND_A) {
        return;
    }

    const struct sockaddr_in *addr4 = (const struct sockaddr_in *)peer_addr;
    if (inet_ntop(AF_INET, &addr4->sin_addr, s_frontend_peer.ip, sizeof(s_frontend_peer.ip)) == NULL) {
        return;
    }

    s_frontend_peer.present = true;
    s_frontend_peer.last_seen_ms = log_utils_uptime_ms();
    s_frontend_peer.http_port = announce->http_port;
}

static esp_err_t process_incoming_frame(const uint8_t *rx_data,
                                        size_t rx_len,
                                        uint8_t *tx_data,
                                        size_t tx_capacity,
                                        size_t *tx_len)
{
    wdap_message_t message = {0};
    ESP_RETURN_ON_ERROR(transport_proto_decode(rx_data, rx_len, &message), TAG, "decode request failed");

    if (message.msg_type == WDAP_MSG_STREAM) {
        *tx_len = 0U;
        if (message.cmd == WDAP_CMD_DEVICE_ANNOUNCE) {
            if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) == pdTRUE) {
                update_frontend_peer_locked(&s_peer_addr, s_peer_addr_len, &message);
                xSemaphoreGive(s_socket_lock);
            }
            return ESP_OK;
        }
        return uart_bridge_handle_message(&message);
    }

    return dap_backend_process_frame(rx_data, rx_len, tx_data, tx_capacity, tx_len);
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "softap started ssid=%s channel=%d", CONFIG_WDAP_WIFI_SSID, CONFIG_WDAP_WIFI_CHANNEL);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "frontend station connected");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGW(TAG, "frontend station disconnected");
    }
}

static int create_socket(void)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed: errno=%d", errno);
        return -1;
    }

    (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &WDAP_SOCKET_BUFFER_BYTES, sizeof(WDAP_SOCKET_BUFFER_BYTES));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &WDAP_SOCKET_BUFFER_BYTES, sizeof(WDAP_SOCKET_BUFFER_BYTES));

    const struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_WDAP_UDP_PORT),
        .sin_addr = {
            .s_addr = htonl(INADDR_ANY),
        },
    };

    if (bind(sock, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "socket bind failed: errno=%d", errno);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "udp server listening on %d", CONFIG_WDAP_UDP_PORT);
    return sock;
}

static void udp_server_task(void *arg)
{
    (void)arg;

    uint8_t rx_buffer[WDAP_MAX_FRAME_SIZE];
    uint8_t tx_buffer[WDAP_MAX_FRAME_SIZE];

    while (true) {
        int sock = create_socket();
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) == pdTRUE) {
            s_socket = sock;
            s_peer_valid = false;
            s_peer_addr_len = 0;
            memset(&s_peer_addr, 0, sizeof(s_peer_addr));
            xSemaphoreGive(s_socket_lock);
        }

        while (true) {
            struct sockaddr_storage peer_addr = {0};
            socklen_t peer_len = sizeof(peer_addr);
            const ssize_t rx_len = recvfrom(sock,
                                            rx_buffer,
                                            sizeof(rx_buffer),
                                            0,
                                            (struct sockaddr *)&peer_addr,
                                            &peer_len);
            if (rx_len < 0) {
                ESP_LOGW(TAG, "recvfrom failed: errno=%d", errno);
                break;
            }

            if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) == pdTRUE) {
                s_peer_addr = peer_addr;
                s_peer_addr_len = peer_len;
                s_peer_valid = true;
                if (rx_len >= (ssize_t)WDAP_PACKET_OVERHEAD) {
                    wdap_message_t message = {0};
                    if (transport_proto_decode(rx_buffer, (size_t)rx_len, &message) == ESP_OK &&
                        message.msg_type == WDAP_MSG_STREAM &&
                        message.cmd == WDAP_CMD_DEVICE_ANNOUNCE) {
                        update_frontend_peer_locked(&peer_addr, peer_len, &message);
                    }
                }
                xSemaphoreGive(s_socket_lock);
            }

            size_t tx_len = 0;
            const esp_err_t err = process_incoming_frame(rx_buffer,
                                                         (size_t)rx_len,
                                                         tx_buffer,
                                                         sizeof(tx_buffer),
                                                         &tx_len);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "request dropped: %s", esp_err_to_name(err));
                continue;
            }

            if (tx_len == 0U) {
                continue;
            }

            const ssize_t sent = sendto(sock,
                                        tx_buffer,
                                        tx_len,
                                        0,
                                        (const struct sockaddr *)&peer_addr,
                                        peer_len);
            if (sent != (ssize_t)tx_len) {
                ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
            }
        }

        if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) == pdTRUE) {
            close_socket_locked();
            xSemaphoreGive(s_socket_lock);
        } else {
            shutdown(sock, 0);
            close(sock);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t wifi_link_init(void)
{
    s_socket_lock = xSemaphoreCreateMutex();
    if (s_socket_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_netif = esp_netif_create_default_wifi_ap();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = CONFIG_WDAP_WIFI_CHANNEL,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    strlcpy((char *)ap_cfg.ap.ssid, CONFIG_WDAP_WIFI_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(CONFIG_WDAP_WIFI_SSID);
    strlcpy((char *)ap_cfg.ap.password, CONFIG_WDAP_WIFI_PASSWORD, sizeof(ap_cfg.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40));

    const BaseType_t ok = xTaskCreatePinnedToCore(udp_server_task,
                                                  "udp_server_b",
                                                  6144,
                                                  NULL,
                                                  5,
                                                  NULL,
                                                  WDAP_SWD_CORE_ID);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_link_send_packet(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_socket_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_socket < 0 || !s_peer_valid) {
        xSemaphoreGive(s_socket_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const ssize_t sent = sendto(s_socket,
                                data,
                                len,
                                0,
                                (const struct sockaddr *)&s_peer_addr,
                                s_peer_addr_len);
    xSemaphoreGive(s_socket_lock);

    if (sent != (ssize_t)len) {
        ESP_LOGW(TAG, "async sendto failed: errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wifi_link_get_local_ip_string(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_wifi_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_wifi_netif, &ip_info), TAG, "get AP ip failed");
    snprintf(buffer, buffer_size, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_link_get_frontend_peer_info(wifi_link_frontend_peer_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_socket_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(info, 0, sizeof(*info));
    if (xSemaphoreTake(s_socket_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    info->present = s_frontend_peer.present;
    info->last_seen_ms = s_frontend_peer.last_seen_ms;
    info->http_port = s_frontend_peer.http_port;
    strlcpy(info->ip, s_frontend_peer.ip, sizeof(info->ip));
    xSemaphoreGive(s_socket_lock);

    if (info->present) {
        const uint32_t age_ms = log_utils_uptime_ms() - info->last_seen_ms;
        info->online = age_ms <= WDAP_FRONTEND_ONLINE_TIMEOUT_MS;
    }
    return ESP_OK;
}
