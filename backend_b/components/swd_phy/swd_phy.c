#include "swd_phy.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "sdkconfig.h"
#include "wdap_protocol.h"

static const char *TAG = "swd_phy";

typedef struct {
    int swclk_gpio;
    int swdio_gpio;
    uint32_t clock_hz;
    uint32_t half_period_us;
    uint32_t half_period_cycles;
    uint8_t turnaround_cycles;
    bool data_phase;
    bool swdio_output_enabled;
    bool initialized;
} swd_phy_state_t;

static swd_phy_state_t s_state;
static gpio_dev_t *const s_gpio_hw = GPIO_LL_GET_HW(0);

static const char *ack_to_string(uint8_t ack)
{
    switch (ack) {
    case WDAP_ACK_OK:
        return "OK";
    case WDAP_ACK_WAIT:
        return "WAIT";
    case WDAP_ACK_FAULT:
        return "FAULT";
    case WDAP_ACK_NONE:
        return "NONE";
    case WDAP_ACK_PARITY:
        return "PARITY";
    case WDAP_ACK_PROTOCOL:
        return "PROTOCOL";
    default:
        return "INVALID";
    }
}

static void log_request_bits(const char *op, uint8_t request, uint8_t addr)
{
#if CONFIG_WDAP_SWD_DIAG_VERBOSE
    ESP_LOGD(TAG,
             "%s req=0x%02x start=%u apndp=%u rnw=%u a2=%u a3=%u parity=%u stop=%u park=%u addr=0x%02x hz=%" PRIu32,
             op,
             request,
             (request >> 0) & 1U,
             (request >> 1) & 1U,
             (request >> 2) & 1U,
             (request >> 3) & 1U,
             (request >> 4) & 1U,
             (request >> 5) & 1U,
             (request >> 6) & 1U,
             (request >> 7) & 1U,
             addr,
             s_state.clock_hz);
#else
    (void)op;
    (void)request;
    (void)addr;
#endif
}

static void log_invalid_ack(const char *op, uint8_t request, uint8_t ack)
{
    const int line_level = gpio_ll_get_level(s_gpio_hw, (uint32_t)s_state.swdio_gpio);
    ESP_LOGW(TAG,
             "%s invalid ack req=0x%02x ack=0x%02x(%s) bits=%u%u%u swdio_level=%d hz=%" PRIu32,
             op,
             request,
             ack,
             ack_to_string(ack),
             (ack >> 2) & 1U,
             (ack >> 1) & 1U,
             (ack >> 0) & 1U,
             line_level,
             s_state.clock_hz);
}

static uint8_t parity32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value &= 0xfU;
    return (uint8_t)((0x6996U >> value) & 1U);
}

static inline void IRAM_ATTR swclk_set_level(uint32_t level)
{
    gpio_ll_set_level(s_gpio_hw, (uint32_t)s_state.swclk_gpio, level);
}

static inline void IRAM_ATTR swdio_set_level(uint32_t level)
{
    gpio_ll_set_level(s_gpio_hw, (uint32_t)s_state.swdio_gpio, level);
}

static void IRAM_ATTR delay_half_period(void)
{
    if (s_state.half_period_cycles > 0U) {
        const uint32_t start = esp_cpu_get_cycle_count();
        while ((uint32_t)(esp_cpu_get_cycle_count() - start) < s_state.half_period_cycles) {
        }
        return;
    }

    esp_rom_delay_us(s_state.half_period_us);
}

static inline void IRAM_ATTR set_swdio_output(int level)
{
    if (!s_state.swdio_output_enabled) {
        gpio_ll_output_enable(s_gpio_hw, (uint32_t)s_state.swdio_gpio);
        s_state.swdio_output_enabled = true;
    }
    swdio_set_level(level ? 1U : 0U);
}

static inline void IRAM_ATTR set_swdio_input(void)
{
    if (s_state.swdio_output_enabled) {
        gpio_ll_output_disable(s_gpio_hw, (uint32_t)s_state.swdio_gpio);
        s_state.swdio_output_enabled = false;
    }
}

static inline void IRAM_ATTR clock_cycle(void)
{
    swclk_set_level(0U);
    delay_half_period();
    swclk_set_level(1U);
    delay_half_period();
}

static inline void IRAM_ATTR write_bit(uint8_t bit)
{
    swdio_set_level(bit ? 1U : 0U);
    clock_cycle();
}

static inline uint8_t IRAM_ATTR read_bit(void)
{
    swclk_set_level(0U);
    delay_half_period();
    const uint8_t value = (uint8_t)gpio_ll_get_level(s_gpio_hw, (uint32_t)s_state.swdio_gpio);
    swclk_set_level(1U);
    delay_half_period();
    return value;
}

static inline void IRAM_ATTR write_bits(uint32_t value, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i) {
        write_bit((uint8_t)((value >> i) & 1U));
    }
}

static inline uint32_t IRAM_ATTR read_bits(uint8_t count)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < count; ++i) {
        value |= (uint32_t)read_bit() << i;
    }
    return value;
}

static inline void IRAM_ATTR turnaround_to_read(void)
{
    set_swdio_input();
    for (uint8_t i = 0; i < s_state.turnaround_cycles; ++i) {
        clock_cycle();
    }
}

static inline void IRAM_ATTR turnaround_to_write(void)
{
    for (uint8_t i = 0; i < s_state.turnaround_cycles; ++i) {
        clock_cycle();
    }
    set_swdio_output(1);
}

static void IRAM_ATTR backoff_after_ack(bool read_request, uint8_t ack)
{
    if (ack == WDAP_ACK_WAIT || ack == WDAP_ACK_FAULT) {
        if (s_state.data_phase && read_request) {
            for (uint32_t i = 0; i < 33U; ++i) {
                clock_cycle();
            }
        }
        turnaround_to_write();
        if (s_state.data_phase && !read_request) {
            swdio_set_level(0U);
            for (uint32_t i = 0; i < 33U; ++i) {
                clock_cycle();
            }
        }
        swdio_set_level(1U);
        return;
    }

    const uint32_t cycles = (uint32_t)s_state.turnaround_cycles + 33U;
    for (uint32_t i = 0; i < cycles; ++i) {
        clock_cycle();
    }
    set_swdio_output(1);
}

static inline uint8_t IRAM_ATTR make_request(bool apndp, bool rnw, uint8_t addr)
{
    const uint8_t a2 = (uint8_t)((addr >> 2) & 0x1U);
    const uint8_t a3 = (uint8_t)((addr >> 3) & 0x1U);
    const uint8_t parity = (uint8_t)((apndp ? 1U : 0U) ^ (rnw ? 1U : 0U) ^ a2 ^ a3);

    return (uint8_t)(0x81U |
                     ((apndp ? 1U : 0U) << 1) |
                     ((rnw ? 1U : 0U) << 2) |
                     (a2 << 3) |
                     (a3 << 4) |
                     (parity << 5));
}

static esp_err_t IRAM_ATTR read_register(bool apndp, uint8_t addr, uint32_t *value, uint8_t *ack)
{
    if (value == NULL || ack == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t request = make_request(apndp, true, addr);
    set_swdio_output(1);
    log_request_bits(apndp ? "READ_AP" : "READ_DP", request, addr);
    write_bits(request, 8);
    turnaround_to_read();

    *ack = (uint8_t)read_bits(3);
    if (*ack != WDAP_ACK_OK) {
        log_invalid_ack(apndp ? "READ_AP" : "READ_DP", request, *ack);
        backoff_after_ack(true, *ack);
        return ESP_FAIL;
    }

    const uint32_t data = read_bits(32);
    const uint8_t parity = (uint8_t)read_bits(1);
    turnaround_to_write();

    if (parity != parity32(data)) {
        *ack = WDAP_ACK_PARITY;
        return ESP_ERR_INVALID_CRC;
    }

    *value = data;
    swdio_set_level(1U);
    return ESP_OK;
}

static esp_err_t IRAM_ATTR write_register(bool apndp, uint8_t addr, uint32_t value, uint8_t *ack)
{
    if (ack == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t request = make_request(apndp, false, addr);
    set_swdio_output(1);
    log_request_bits(apndp ? "WRITE_AP" : "WRITE_DP", request, addr);
    write_bits(request, 8);
    turnaround_to_read();

    *ack = (uint8_t)read_bits(3);
    if (*ack != WDAP_ACK_OK) {
        log_invalid_ack(apndp ? "WRITE_AP" : "WRITE_DP", request, *ack);
        backoff_after_ack(false, *ack);
        return ESP_FAIL;
    }

    turnaround_to_write();
    write_bits(value, 32);
    write_bit(parity32(value));
    swdio_set_level(1U);
    return ESP_OK;
}

esp_err_t swd_phy_init(const swd_phy_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.swclk_gpio = config->swclk_gpio;
    s_state.swdio_gpio = config->swdio_gpio;

    const gpio_config_t swclk_cfg = {
        .pin_bit_mask = 1ULL << config->swclk_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t swdio_cfg = {
        .pin_bit_mask = 1ULL << config->swdio_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&swclk_cfg));
    ESP_ERROR_CHECK(gpio_config(&swdio_cfg));
    gpio_ll_input_enable(s_gpio_hw, (uint32_t)s_state.swclk_gpio);
    gpio_ll_input_enable(s_gpio_hw, (uint32_t)s_state.swdio_gpio);
    gpio_ll_output_enable(s_gpio_hw, (uint32_t)s_state.swclk_gpio);
    gpio_ll_output_enable(s_gpio_hw, (uint32_t)s_state.swdio_gpio);
    gpio_ll_od_disable(s_gpio_hw, (uint32_t)s_state.swclk_gpio);
    gpio_ll_od_disable(s_gpio_hw, (uint32_t)s_state.swdio_gpio);
    swclk_set_level(1U);
    swdio_set_level(1U);
    s_state.swdio_output_enabled = true;

    ESP_ERROR_CHECK(swd_phy_set_clock(config->clock_hz));
    ESP_ERROR_CHECK(swd_phy_set_transfer_config(1, 0));
    s_state.initialized = true;

    ESP_LOGI(TAG, "real swd phy ready swclk=%d swdio=%d hz=%" PRIu32,
             s_state.swclk_gpio,
             s_state.swdio_gpio,
             s_state.clock_hz);
    return ESP_OK;
}

esp_err_t swd_phy_set_clock(uint32_t hz)
{
    if (hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    s_state.clock_hz = hz;
    s_state.half_period_us = 500000U / hz;
    if (s_state.half_period_us == 0U) {
        s_state.half_period_us = 1U;
    }
    s_state.half_period_cycles = (uint32_t)(((uint64_t)esp_rom_get_cpu_ticks_per_us() * 500000ULL) / hz);
    if (s_state.half_period_cycles == 0U) {
        s_state.half_period_cycles = 1U;
    }
    return ESP_OK;
}

esp_err_t swd_phy_set_transfer_config(uint8_t turnaround, uint8_t data_phase)
{
    if (turnaround == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    s_state.turnaround_cycles = turnaround;
    s_state.data_phase = data_phase != 0U;
    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_phy_write_idle_bits(uint8_t bit, uint32_t count)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    set_swdio_output(bit ? 1 : 0);
    for (uint32_t i = 0; i < count; ++i) {
        clock_cycle();
    }
    swdio_set_level(1U);
    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_phy_drive_pins(uint8_t value, uint8_t select)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if ((select & WDAP_SWJ_PIN_SWCLK_TCK) != 0U) {
        swclk_set_level((value & WDAP_SWJ_PIN_SWCLK_TCK) != 0U ? 1U : 0U);
    }

    if ((select & WDAP_SWJ_PIN_SWDIO_TMS) != 0U) {
        set_swdio_output((value & WDAP_SWJ_PIN_SWDIO_TMS) != 0U ? 1 : 0);
    }

    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_phy_read_pins(uint8_t *pins)
{
    if (!s_state.initialized || pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t value = 0;
    if (gpio_ll_get_level(s_gpio_hw, (uint32_t)s_state.swclk_gpio) != 0) {
        value |= WDAP_SWJ_PIN_SWCLK_TCK;
    }
    if (gpio_ll_get_level(s_gpio_hw, (uint32_t)s_state.swdio_gpio) != 0) {
        value |= WDAP_SWJ_PIN_SWDIO_TMS;
    }

    *pins = value;
    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_phy_swd_sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t count_bits = info & 0x3FU;
    if (count_bits == 0U) {
        count_bits = 64U;
    }

    const uint32_t count_bytes = (count_bits + 7U) / 8U;
    const bool capture = (info & 0x80U) != 0U;

    if (capture) {
        if (swdi == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        memset(swdi, 0, count_bytes);
        set_swdio_input();
        for (uint32_t bit = 0; bit < count_bits; ++bit) {
            const uint8_t bit_val = read_bit();
            swdi[bit / 8U] |= (uint8_t)(bit_val << (bit % 8U));
        }
    } else {
        if (swdo == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        set_swdio_output(1);
        for (uint32_t bit = 0; bit < count_bits; ++bit) {
            const uint8_t bit_val = (uint8_t)((swdo[bit / 8U] >> (bit % 8U)) & 1U);
            write_bit(bit_val);
        }
    }

    swdio_set_level(1U);
    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_phy_swj_sequence(uint32_t count, const uint8_t *data)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    set_swdio_output(0);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t byte_val = data[i / 8U];
        const uint8_t bit_val = (byte_val >> (i % 8U)) & 1U;
        swdio_set_level(bit_val);
        clock_cycle();
    }
    swdio_set_level(1U);
    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_phy_line_reset(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    set_swdio_output(1);
    for (int i = 0; i < 64; ++i) {
        clock_cycle();
    }
    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_phy_jtag_to_swd(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    set_swdio_output(1);
    write_bits(0xe79eU, 16);
    swdio_set_level(1U);
    return ESP_OK;
}

esp_err_t IRAM_ATTR swd_phy_read_dp(uint8_t addr, uint32_t *value, uint8_t *ack)
{
    return read_register(false, addr, value, ack);
}

esp_err_t IRAM_ATTR swd_phy_write_dp(uint8_t addr, uint32_t value, uint8_t *ack)
{
    return write_register(false, addr, value, ack);
}

esp_err_t IRAM_ATTR swd_phy_read_ap(uint8_t addr, uint32_t *value, uint8_t *ack)
{
    return read_register(true, addr, value, ack);
}

esp_err_t IRAM_ATTR swd_phy_write_ap(uint8_t addr, uint32_t value, uint8_t *ack)
{
    return write_register(true, addr, value, ack);
}
