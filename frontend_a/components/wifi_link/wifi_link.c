#include "wifi_link.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include "wdap_protocol.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi_link_a";
static EventGroupHandle_t s_event_group;
static int s_socket = -1;
static TaskHandle_t s_rx_task_handle;
static wifi_link_rx_cb_t s_callback;
static void *s_callback_ctx;

static const int WIFI_CONNECTED_BIT = BIT0;
static const int WDAP_SOCKET_BUFFER_BYTES = (int)(WDAP_MAX_FRAME_SIZE * 8U);
static const BaseType_t WDAP_NET_CORE_ID = 0;

static void close_socket(void)
{
    if (s_socket >= 0) {
        shutdown(s_socket, 0);
        close(s_socket);
        s_socket = -1;
    }
}

static esp_err_t ensure_socket(void)
{
    if (s_socket >= 0) {
        return ESP_OK;
    }

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed: errno=%d", errno);
        return ESP_FAIL;
    }

    (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &WDAP_SOCKET_BUFFER_BYTES, sizeof(WDAP_SOCKET_BUFFER_BYTES));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &WDAP_SOCKET_BUFFER_BYTES, sizeof(WDAP_SOCKET_BUFFER_BYTES));

    struct timeval timeout = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in peer_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_WDAP_UDP_PORT),
    };
    if (inet_pton(AF_INET, CONFIG_WDAP_BACKEND_IP, &peer_addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "invalid backend ip: %s", CONFIG_WDAP_BACKEND_IP);
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    if (connect(sock, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) != 0) {
        ESP_LOGE(TAG, "socket connect failed: errno=%d", errno);
        close(sock);
        return ESP_FAIL;
    }

    s_socket = sock;
    ESP_LOGI(TAG, "udp peer ready: %s:%d", CONFIG_WDAP_BACKEND_IP, CONFIG_WDAP_UDP_PORT);
    return ESP_OK;
}

static void rx_task(void *arg)
{
    (void)arg;

    uint8_t buffer[WDAP_MAX_FRAME_SIZE];
    while (true) {
        if (!wifi_link_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        const ssize_t received = recv(s_socket, buffer, sizeof(buffer), 0);
        if (received > 0) {
            if (s_callback != NULL) {
                s_callback(buffer, (size_t)received, s_callback_ctx);
            }
            continue;
        }

        if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recv failed: errno=%d, reconnecting socket", errno);
            close_socket();
        }
    }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        close_socket();
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        ESP_ERROR_CHECK(esp_wifi_connect());
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "wifi connected to backend ap");
        if (ensure_socket() != ESP_OK) {
            ESP_LOGW(TAG, "socket setup will retry on next send");
        }
    }
}

esp_err_t wifi_link_init(wifi_link_rx_cb_t callback, void *ctx)
{
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_callback = callback;
    s_callback_ctx = ctx;

    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_WDAP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_WDAP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40));

    const BaseType_t ok = xTaskCreatePinnedToCore(rx_task,
                                                  "wifi_rx_a",
                                                  4096,
                                                  NULL,
                                                  5,
                                                  &s_rx_task_handle,
                                                  WDAP_NET_CORE_ID);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool wifi_link_is_ready(void)
{
    const EventBits_t bits = xEventGroupGetBits(s_event_group);
    return ((bits & WIFI_CONNECTED_BIT) != 0) && (s_socket >= 0);
}

esp_err_t wifi_link_send_packet(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((xEventGroupGetBits(s_event_group) & WIFI_CONNECTED_BIT) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ensure_socket() != ESP_OK) {
        return ESP_FAIL;
    }

    const ssize_t sent = send(s_socket, data, len, 0);
    if (sent != (ssize_t)len) {
        ESP_LOGW(TAG, "send failed: expected=%u actual=%d errno=%d", (unsigned)len, (int)sent, errno);
        close_socket();
        return ESP_FAIL;
    }

    return ESP_OK;
}
