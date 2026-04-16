#include "board_support.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

static const char *TAG = "board_support";

#ifdef CONFIG_WDAP_TARGET_RESET_SETTLE_MS
#define WDAP_TARGET_RESET_SETTLE_MS CONFIG_WDAP_TARGET_RESET_SETTLE_MS
#else
#define WDAP_TARGET_RESET_SETTLE_MS 5U
#endif

static board_support_pins_t s_pins = {
    .swclk_gpio = CONFIG_WDAP_SWD_SWCLK_GPIO,
    .swdio_gpio = CONFIG_WDAP_SWD_SWDIO_GPIO,
    .nreset_gpio = CONFIG_WDAP_TARGET_NRST_GPIO,
};

esp_err_t board_support_init(void)
{
    const gpio_config_t nreset_cfg = {
        .pin_bit_mask = 1ULL << s_pins.nreset_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&nreset_cfg));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)s_pins.nreset_gpio, 1));

    ESP_LOGI(TAG, "pins swclk=%d swdio=%d nreset=%d",
             s_pins.swclk_gpio,
             s_pins.swdio_gpio,
             s_pins.nreset_gpio);
    return ESP_OK;
}

const board_support_pins_t *board_support_get_pins(void)
{
    return &s_pins;
}

esp_err_t board_support_target_reset_pulse(uint32_t pulse_ms)
{
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)s_pins.nreset_gpio, 0));
    esp_rom_delay_us(pulse_ms * 1000U);
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)s_pins.nreset_gpio, 1));
    esp_rom_delay_us(WDAP_TARGET_RESET_SETTLE_MS * 1000U);
    return ESP_OK;
}
