#include "wdap_runtime.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "wdap_runtime";

typedef struct {
    portMUX_TYPE lock;
    const char *role;
    bool busy;
    char busy_reason[24];
} wdap_runtime_state_t;

static wdap_runtime_state_t s_state = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .role = "unknown",
    .busy = false,
    .busy_reason = {0},
};

esp_err_t wdap_runtime_init(const char *role)
{
    if (role == NULL || role[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state.lock);
    s_state.role = role;
    s_state.busy = false;
    s_state.busy_reason[0] = '\0';
    taskEXIT_CRITICAL(&s_state.lock);
    return ESP_OK;
}

const char *wdap_runtime_get_role(void)
{
    return s_state.role;
}

const char *wdap_runtime_get_version(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc == NULL || app_desc->version[0] == '\0') {
        return "unknown";
    }
    return app_desc->version;
}

const char *wdap_runtime_get_busy_reason(void)
{
    return s_state.busy_reason[0] != '\0' ? s_state.busy_reason : "idle";
}

bool wdap_runtime_is_busy(void)
{
    bool busy = false;

    taskENTER_CRITICAL(&s_state.lock);
    busy = s_state.busy;
    taskEXIT_CRITICAL(&s_state.lock);
    return busy;
}

esp_err_t wdap_runtime_try_acquire_busy(const char *reason)
{
    if (reason == NULL || reason[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    taskENTER_CRITICAL(&s_state.lock);
    if (s_state.busy) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_state.busy = true;
        strlcpy(s_state.busy_reason, reason, sizeof(s_state.busy_reason));
    }
    taskEXIT_CRITICAL(&s_state.lock);
    return err;
}

void wdap_runtime_release_busy(void)
{
    taskENTER_CRITICAL(&s_state.lock);
    s_state.busy = false;
    s_state.busy_reason[0] = '\0';
    taskEXIT_CRITICAL(&s_state.lock);
}

esp_err_t wdap_runtime_get_running_partition_label(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *partition = esp_ota_get_running_partition();
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_FAIL, TAG, "running partition unavailable");
    strlcpy(buffer, partition->label, buffer_size);
    return ESP_OK;
}

esp_err_t wdap_runtime_mark_running_partition_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_RETURN_ON_FALSE(running != NULL, ESP_FAIL, TAG, "running partition unavailable");

    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    ESP_RETURN_ON_ERROR(esp_ota_get_state_partition(running, &ota_state), TAG, "get OTA state failed");

    if (ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "running partition %s pending verify, marking valid", running->label);
    return esp_ota_mark_app_valid_cancel_rollback();
}
