#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int swclk_gpio;
    int swdio_gpio;
    int nreset_gpio;
} board_support_pins_t;

esp_err_t board_support_init(void);
const board_support_pins_t *board_support_get_pins(void);
esp_err_t board_support_target_reset_pulse(uint32_t pulse_ms);
esp_err_t board_support_target_reset_drive(bool asserted);
bool board_support_target_reset_is_asserted(void);
