#include "host_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "host_link";
static const BaseType_t HOST_LINK_CORE_ID = 1;

static host_link_line_cb_t s_callback;
static void *s_callback_ctx;

static void host_link_task(void *arg)
{
    (void)arg;

    char line[128];
    bool prompt_visible = false;
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\nwireless-dap frontend console\n");
    printf("commands: help, ping, version, caps, line_reset, target_reset, halt, read_dp_idcode, read_dp <addr>, write_dp <addr> <value>, read_ap <addr>, write_ap <addr> <value>, set_freq <hz>\n");

    while (true) {
        if (!prompt_visible) {
            printf("wdap> ");
            prompt_visible = true;
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        prompt_visible = false;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        if (s_callback != NULL) {
            s_callback(line, s_callback_ctx);
        } else {
            ESP_LOGW(TAG, "host callback is not registered");
        }
    }
}

esp_err_t host_link_start(host_link_line_cb_t callback, void *ctx)
{
    s_callback = callback;
    s_callback_ctx = ctx;

    const BaseType_t ok = xTaskCreatePinnedToCore(host_link_task,
                                                  "host_link",
                                                  4096,
                                                  NULL,
                                                  4,
                                                  NULL,
                                                  HOST_LINK_CORE_ID);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}
