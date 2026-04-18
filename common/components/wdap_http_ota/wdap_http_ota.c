#include "wdap_http_ota.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wdap_runtime.h"

static const char *TAG = "wdap_http_ota";

#define WDAP_HTTP_OTA_BUFFER_SIZE 1024U
#define WDAP_HTTP_RESTART_DELAY_MS 1200U
#define WDAP_HTTP_JSON_BUFFER_SIZE 512U

typedef struct {
    httpd_handle_t server;
    wdap_http_ota_config_t config;
    bool restart_pending;
} wdap_http_ota_state_t;

static wdap_http_ota_state_t s_state;

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_json_response(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *message)
{
    char body[192];

    snprintf(body,
             sizeof(body),
             "{\"ok\":false,\"error\":\"%s\"}",
             message != NULL ? message : "unknown");
    return send_json_response(req, status, body);
}

static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    set_cors_headers(req);
    return httpd_resp_send(req, NULL, 0);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WDAP_HTTP_RESTART_DELAY_MS));
    esp_restart();
}

static void schedule_restart(void)
{
    if (s_state.restart_pending) {
        return;
    }

    s_state.restart_pending = true;
    if (xTaskCreate(restart_task, "wdap_restart", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create restart task, restarting immediately");
        esp_restart();
    }
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    set_cors_headers(req);
    return httpd_resp_sendstr(req, s_state.config.index_html);
}

static esp_err_t info_handler(httpd_req_t *req)
{
    char ip[16] = "0.0.0.0";
    char partition[20] = "unknown";
    char body[WDAP_HTTP_JSON_BUFFER_SIZE];

    if (s_state.config.get_ip != NULL) {
        (void)s_state.config.get_ip(ip, sizeof(ip));
    }
    (void)wdap_runtime_get_running_partition_label(partition, sizeof(partition));

    snprintf(body,
             sizeof(body),
             "{\"role\":\"%s\",\"version\":\"%s\",\"ip\":\"%s\",\"running_partition\":\"%s\","
             "\"ota_supported\":true,\"busy\":%s,\"busy_reason\":\"%s\"}",
             wdap_runtime_get_role(),
             wdap_runtime_get_version(),
             ip,
             partition,
             wdap_runtime_is_busy() ? "true" : "false",
             wdap_runtime_get_busy_reason());
    return send_json_response(req, "200 OK", body);
}

static esp_err_t devices_handler(httpd_req_t *req)
{
    char body[WDAP_HTTP_JSON_BUFFER_SIZE];
    size_t written_len = 0U;

    if (s_state.config.build_devices_json == NULL) {
        return send_error_json(req, "404 Not Found", "devices endpoint disabled");
    }

    const esp_err_t err = s_state.config.build_devices_json(body, sizeof(body), &written_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build devices JSON failed: %s", esp_err_to_name(err));
        return send_error_json(req, "500 Internal Server Error", "devices endpoint failed");
    }
    if (written_len == 0U || written_len >= sizeof(body)) {
        return send_error_json(req, "500 Internal Server Error", "devices payload too large");
    }

    return send_json_response(req, "200 OK", body);
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (wdap_runtime_try_acquire_busy("ota") != ESP_OK) {
        return send_error_json(req, "409 Conflict", "device busy");
    }

    esp_err_t err = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    char buffer[WDAP_HTTP_OTA_BUFFER_SIZE];

    if (update_partition == NULL) {
        wdap_runtime_release_busy();
        return send_error_json(req, "500 Internal Server Error", "no update partition");
    }

    if (req->content_len <= 0) {
        wdap_runtime_release_busy();
        return send_error_json(req, "400 Bad Request", "empty upload");
    }

    if ((size_t)req->content_len > update_partition->size) {
        wdap_runtime_release_busy();
        return send_error_json(req, "413 Payload Too Large", "firmware too large");
    }

    err = esp_ota_begin(update_partition, (size_t)req->content_len, &ota_handle);
    if (err != ESP_OK) {
        wdap_runtime_release_busy();
        return send_error_json(req, "500 Internal Server Error", "esp_ota_begin failed");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        const int chunk_len = remaining > (int)sizeof(buffer) ? (int)sizeof(buffer) : remaining;
        const int received = httpd_req_recv(req, buffer, chunk_len);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            err = ESP_FAIL;
            break;
        }

        err = esp_ota_write(ota_handle, buffer, (size_t)received);
        if (err != ESP_OK) {
            break;
        }
        remaining -= received;
    }

    if (err == ESP_OK) {
        err = esp_ota_end(ota_handle);
    } else {
        esp_ota_abort(ota_handle);
    }
    ota_handle = 0;

    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update_partition);
    }

    if (err != ESP_OK) {
        wdap_runtime_release_busy();
        ESP_LOGE(TAG, "OTA upload failed: %s", esp_err_to_name(err));
        return send_error_json(req, "500 Internal Server Error", "OTA apply failed");
    }

    char body[256];
    snprintf(body,
             sizeof(body),
             "{\"ok\":true,\"next_partition\":\"%s\",\"restart_ms\":%u}",
             update_partition->label,
             (unsigned)WDAP_HTTP_RESTART_DELAY_MS);
    const esp_err_t send_err = send_json_response(req, "200 OK", body);
    schedule_restart();
    return send_err;
}

esp_err_t wdap_http_ota_start(const wdap_http_ota_config_t *config)
{
    if (config == NULL || config->get_ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.server != NULL) {
        return ESP_OK;
    }

    s_state.config = *config;

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = 80;
    http_config.ctrl_port = 32768;
    http_config.stack_size = 8192;
    http_config.max_uri_handlers = 8;
    http_config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_state.server, &http_config), TAG, "httpd_start failed");

    const httpd_uri_t info_get = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = info_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t info_options = {
        .uri = "/api/info",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t ota_post = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t ota_options = {
        .uri = "/api/ota",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.server, &info_get), TAG, "register /api/info failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.server, &info_options), TAG, "register /api/info OPTIONS failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.server, &ota_post), TAG, "register /api/ota failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.server, &ota_options), TAG, "register /api/ota OPTIONS failed");

    if (config->index_html != NULL) {
        const httpd_uri_t index_get = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL,
        };
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.server, &index_get), TAG, "register / failed");
    }

    if (config->build_devices_json != NULL) {
        const httpd_uri_t devices_get = {
            .uri = "/api/devices",
            .method = HTTP_GET,
            .handler = devices_handler,
            .user_ctx = NULL,
        };
        const httpd_uri_t devices_options = {
            .uri = "/api/devices",
            .method = HTTP_OPTIONS,
            .handler = options_handler,
            .user_ctx = NULL,
        };
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.server, &devices_get), TAG, "register /api/devices failed");
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_state.server, &devices_options), TAG, "register /api/devices OPTIONS failed");
    }

    ESP_LOGI(TAG, "HTTP OTA server ready on port %d", http_config.server_port);
    return ESP_OK;
}
