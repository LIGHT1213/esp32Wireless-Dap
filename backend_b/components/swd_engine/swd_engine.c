#include "swd_engine.h"

#include "board_support.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "swd_phy.h"
#include "wdap_protocol.h"

static const char *TAG = "swd_engine";
static const uint32_t MOCK_DP_IDCODE = 0x2ba01477UL;
static const uint8_t DP_ABORT_ADDR = 0x00U;
static const uint8_t DP_CTRL_STAT_ADDR = 0x04U;
static const uint8_t DP_SELECT_ADDR = 0x08U;
static const uint8_t DP_RDBUFF_ADDR = 0x0cU;
static const uint8_t AP_CSW_ADDR = 0x00U;
static const uint8_t AP_TAR_ADDR = 0x04U;
static const uint8_t AP_DRW_ADDR = 0x0cU;
static const uint32_t APBANKSEL_MASK = 0x000000f0UL;
static const uint32_t DAPABORT = 0x00000001UL;
static const uint32_t STKCMPCLR = 0x00000002UL;
static const uint32_t STKERRCLR = 0x00000004UL;
static const uint32_t WDERRCLR = 0x00000008UL;
static const uint32_t ORUNERRCLR = 0x00000010UL;
static const uint32_t MASKLANE = 0x00000f00UL;
static const uint32_t CDBGPWRUPREQ = 0x10000000UL;
static const uint32_t CDBGPWRUPACK = 0x20000000UL;
static const uint32_t CSYSPWRUPREQ = 0x40000000UL;
static const uint32_t CSYSPWRUPACK = 0x80000000UL;
static const uint32_t CSW_SIZE32 = 0x00000002UL;
static const uint32_t CSW_SADDRINC = 0x00000010UL;
static const uint32_t CSW_DBGSTAT = 0x00000040UL;
static const uint32_t CSW_HPROT = 0x02000000UL;
static const uint32_t CSW_RESERVED = 0x01000000UL;
static const uint32_t CSW_MSTRDBG = 0x20000000UL;
static const uint32_t CSW_VALUE = CSW_RESERVED | CSW_MSTRDBG | CSW_HPROT | CSW_DBGSTAT | CSW_SADDRINC;
static const uint32_t DBG_HCSR = 0xE000EDF0UL;
static const uint32_t DBGKEY = 0xA05F0000UL;
static const uint32_t C_DEBUGEN = 0x00000001UL;
static const uint32_t C_HALT = 0x00000002UL;
static const uint32_t S_HALT = 0x00020000UL;
static const int HALT_POLL_RETRIES = 50;

typedef struct {
    bool mock_mode;
    bool link_synchronized;
    bool debug_powered;
    uint32_t dp_select;
    uint32_t default_hz;
    uint32_t current_hz;
} swd_engine_state_t;

static swd_engine_state_t s_state;

static esp_err_t ack_error_to_esp(uint8_t ack, esp_err_t fallback)
{
    switch (ack) {
    case WDAP_ACK_OK:
        return ESP_OK;
    case WDAP_ACK_WAIT:
        return ESP_ERR_TIMEOUT;
    case WDAP_ACK_FAULT:
        return ESP_FAIL;
    case WDAP_ACK_PARITY:
    case WDAP_ACK_PROTOCOL:
        return ESP_ERR_INVALID_CRC;
    default:
        return fallback;
    }
}

static esp_err_t swd_dp_write(uint8_t addr, uint32_t value, uint8_t *ack_out)
{
    uint8_t ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_phy_write_dp(addr, value, &ack);
    if (ack_out != NULL) {
        *ack_out = ack;
    }
    if (err == ESP_OK && addr == DP_SELECT_ADDR) {
        s_state.dp_select = value;
    }
    return ack_error_to_esp(ack, err);
}

static esp_err_t swd_dp_read(uint8_t addr, uint32_t *value, uint8_t *ack_out)
{
    uint8_t ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_phy_read_dp(addr, value, &ack);
    if (ack_out != NULL) {
        *ack_out = ack;
    }
    return ack_error_to_esp(ack, err);
}

static esp_err_t swd_select_ap_bank(uint8_t addr)
{
    const uint32_t select = (uint32_t)(addr & APBANKSEL_MASK);
    if (s_state.dp_select == select) {
        return ESP_OK;
    }

    return swd_dp_write(DP_SELECT_ADDR, select, NULL);
}

static esp_err_t swd_clear_errors(void)
{
    return swd_dp_write(DP_ABORT_ADDR,
                        DAPABORT | STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR,
                        NULL);
}

static esp_err_t swd_debug_power_up(void)
{
    if (s_state.mock_mode || s_state.debug_powered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(swd_clear_errors(), TAG, "clear dp errors failed");
    ESP_RETURN_ON_ERROR(swd_dp_write(DP_SELECT_ADDR, 0, NULL), TAG, "select dp bank 0 failed");
    ESP_RETURN_ON_ERROR(swd_dp_write(DP_CTRL_STAT_ADDR,
                                     CSYSPWRUPREQ | CDBGPWRUPREQ | MASKLANE,
                                     NULL),
                        TAG,
                        "request debug power-up failed");

    for (int i = 0; i < HALT_POLL_RETRIES; ++i) {
        uint32_t ctrl_stat = 0;
        ESP_RETURN_ON_ERROR(swd_dp_read(DP_CTRL_STAT_ADDR, &ctrl_stat, NULL), TAG, "read ctrl/stat failed");
        if ((ctrl_stat & (CDBGPWRUPACK | CSYSPWRUPACK)) == (CDBGPWRUPACK | CSYSPWRUPACK)) {
            s_state.debug_powered = true;
            return ESP_OK;
        }
        esp_rom_delay_us(1000);
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t swd_ap_read_raw(uint8_t addr, uint32_t *value, uint8_t *ack_out)
{
    uint8_t ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_phy_read_ap(addr, value, &ack);
    if (ack_out != NULL) {
        *ack_out = ack;
    }
    return ack_error_to_esp(ack, err);
}

static esp_err_t swd_ap_write_raw(uint8_t addr, uint32_t value, uint8_t *ack_out)
{
    uint8_t ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_phy_write_ap(addr, value, &ack);
    if (ack_out != NULL) {
        *ack_out = ack;
    }
    return ack_error_to_esp(ack, err);
}

static esp_err_t swd_memap_write_word(uint32_t addr, uint32_t value)
{
    ESP_RETURN_ON_ERROR(swd_debug_power_up(), TAG, "debug power-up failed");
    ESP_RETURN_ON_ERROR(swd_engine_write_ap(AP_CSW_ADDR, CSW_VALUE | CSW_SIZE32, NULL), TAG, "write CSW failed");
    ESP_RETURN_ON_ERROR(swd_engine_write_ap(AP_TAR_ADDR, addr, NULL), TAG, "write TAR failed");
    return swd_engine_write_ap(AP_DRW_ADDR, value, NULL);
}

static esp_err_t swd_memap_read_word(uint32_t addr, uint32_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(swd_debug_power_up(), TAG, "debug power-up failed");
    ESP_RETURN_ON_ERROR(swd_engine_write_ap(AP_CSW_ADDR, CSW_VALUE | CSW_SIZE32, NULL), TAG, "write CSW failed");
    ESP_RETURN_ON_ERROR(swd_engine_write_ap(AP_TAR_ADDR, addr, NULL), TAG, "write TAR failed");
    return swd_engine_read_ap(AP_DRW_ADDR, value, NULL);
}

static esp_err_t ensure_link_ready(void)
{
    if (s_state.mock_mode || s_state.link_synchronized) {
        return ESP_OK;
    }

    return swd_engine_line_reset(NULL);
}

esp_err_t swd_engine_init(void)
{
#ifdef CONFIG_WDAP_BACKEND_MOCK_SWD
    s_state.mock_mode = CONFIG_WDAP_BACKEND_MOCK_SWD;
#else
    s_state.mock_mode = false;
#endif
    s_state.default_hz = CONFIG_WDAP_SWD_DEFAULT_HZ;
    s_state.current_hz = CONFIG_WDAP_SWD_DEFAULT_HZ;
    s_state.link_synchronized = false;
    s_state.debug_powered = false;
    s_state.dp_select = UINT32_MAX;

    if (s_state.mock_mode) {
        ESP_LOGW(TAG, "mock swd mode is enabled, hardware access is bypassed");
        return ESP_OK;
    }

    const board_support_pins_t *pins = board_support_get_pins();
    const swd_phy_config_t phy_cfg = {
        .swclk_gpio = pins->swclk_gpio,
        .swdio_gpio = pins->swdio_gpio,
        .clock_hz = s_state.current_hz,
    };
    return swd_phy_init(&phy_cfg);
}

esp_err_t swd_engine_set_frequency(uint32_t hz)
{
    if (hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    s_state.current_hz = hz;
    if (s_state.mock_mode) {
        return ESP_OK;
    }

    return swd_phy_set_clock(hz);
}

esp_err_t swd_engine_line_reset(uint8_t *ack)
{
    if (ack != NULL) {
        *ack = WDAP_ACK_OK;
    }

    if (s_state.mock_mode) {
        s_state.link_synchronized = true;
        s_state.debug_powered = true;
        return ESP_OK;
    }

    esp_err_t err = swd_phy_line_reset();
    if (err == ESP_OK) {
        err = swd_phy_jtag_to_swd();
    }
    if (err == ESP_OK) {
        err = swd_phy_line_reset();
    }

    s_state.link_synchronized = (err == ESP_OK);
    if (err == ESP_OK) {
        s_state.debug_powered = false;
        s_state.dp_select = UINT32_MAX;
    }
    return err;
}

esp_err_t swd_engine_target_reset(void)
{
    s_state.link_synchronized = false;
    s_state.debug_powered = false;
    s_state.dp_select = UINT32_MAX;
    return board_support_target_reset_pulse(CONFIG_WDAP_TARGET_RESET_PULSE_MS);
}

esp_err_t swd_engine_target_halt(uint32_t *dhcsr)
{
    if (s_state.mock_mode) {
        if (dhcsr != NULL) {
            *dhcsr = DBGKEY | C_DEBUGEN | C_HALT | S_HALT;
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_link_ready(), TAG, "swd link not ready");
    ESP_RETURN_ON_ERROR(swd_memap_write_word(DBG_HCSR, DBGKEY | C_DEBUGEN | C_HALT), TAG, "write DHCSR failed");

    for (int i = 0; i < HALT_POLL_RETRIES; ++i) {
        uint32_t value = 0;
        ESP_RETURN_ON_ERROR(swd_memap_read_word(DBG_HCSR, &value), TAG, "read DHCSR failed");
        if (dhcsr != NULL) {
            *dhcsr = value;
        }
        if ((value & S_HALT) != 0U) {
            return ESP_OK;
        }
        esp_rom_delay_us(1000);
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t swd_engine_read_dp(uint8_t addr, uint32_t *value, uint8_t *ack)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state.mock_mode) {
        if (ack != NULL) {
            *ack = WDAP_ACK_OK;
        }
        *value = (addr == 0x00U) ? MOCK_DP_IDCODE : 0;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_link_ready(), TAG, "swd link not ready");

    uint8_t phy_ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_phy_read_dp(addr, value, &phy_ack);
    if (ack != NULL) {
        *ack = phy_ack;
    }
    return ack_error_to_esp(phy_ack, err);
}

esp_err_t swd_engine_write_dp(uint8_t addr, uint32_t value, uint8_t *ack)
{
    if (s_state.mock_mode) {
        if (ack != NULL) {
            *ack = WDAP_ACK_OK;
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_link_ready(), TAG, "swd link not ready");

    uint8_t phy_ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_phy_write_dp(addr, value, &phy_ack);
    if (ack != NULL) {
        *ack = phy_ack;
    }
    return ack_error_to_esp(phy_ack, err);
}

esp_err_t swd_engine_read_ap(uint8_t addr, uint32_t *value, uint8_t *ack)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state.mock_mode) {
        if (ack != NULL) {
            *ack = WDAP_ACK_OK;
        }
        *value = 0;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_link_ready(), TAG, "swd link not ready");
    ESP_RETURN_ON_ERROR(swd_debug_power_up(), TAG, "debug power-up failed");
    ESP_RETURN_ON_ERROR(swd_select_ap_bank(addr), TAG, "select ap bank failed");

    uint32_t dummy = 0;
    uint8_t ap_ack = WDAP_ACK_NONE;
    ESP_RETURN_ON_ERROR(swd_ap_read_raw(addr, &dummy, &ap_ack), TAG, "prime ap read failed");

    uint32_t result = 0;
    uint8_t dp_ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_dp_read(DP_RDBUFF_ADDR, &result, &dp_ack);
    if (ack != NULL) {
        *ack = dp_ack;
    }
    if (err != ESP_OK) {
        return err;
    }

    *value = result;
    return ESP_OK;
}

esp_err_t swd_engine_write_ap(uint8_t addr, uint32_t value, uint8_t *ack)
{
    if (s_state.mock_mode) {
        if (ack != NULL) {
            *ack = WDAP_ACK_OK;
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_link_ready(), TAG, "swd link not ready");
    ESP_RETURN_ON_ERROR(swd_debug_power_up(), TAG, "debug power-up failed");
    ESP_RETURN_ON_ERROR(swd_select_ap_bank(addr), TAG, "select ap bank failed");

    uint8_t ap_ack = WDAP_ACK_NONE;
    ESP_RETURN_ON_ERROR(swd_ap_write_raw(addr, value, &ap_ack), TAG, "ap write failed");

    uint32_t flush = 0;
    uint8_t dp_ack = WDAP_ACK_NONE;
    const esp_err_t err = swd_dp_read(DP_RDBUFF_ADDR, &flush, &dp_ack);
    if (ack != NULL) {
        *ack = dp_ack;
    }
    return err;
}

bool swd_engine_is_mock_mode(void)
{
    return s_state.mock_mode;
}

uint32_t swd_engine_get_default_frequency(void)
{
    return s_state.default_hz;
}

uint32_t swd_engine_get_current_frequency(void)
{
    return s_state.current_hz;
}

uint32_t swd_engine_get_capabilities(void)
{
    uint32_t flags = WDAP_CAP_PING |
                     WDAP_CAP_LINE_RESET |
                     WDAP_CAP_READ_DP |
                     WDAP_CAP_WRITE_DP |
                     WDAP_CAP_TARGET_RESET |
                     WDAP_CAP_HEARTBEAT |
                     WDAP_CAP_AP_ACCESS |
                     WDAP_CAP_TARGET_HALT;

    if (s_state.mock_mode) {
        flags |= WDAP_CAP_MOCK_SWD;
    }
    return flags;
}
