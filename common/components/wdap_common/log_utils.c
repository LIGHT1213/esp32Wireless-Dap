#include "log_utils.h"

#include "esp_timer.h"

const char *log_utils_bool_str(bool value)
{
    return value ? "true" : "false";
}

uint32_t log_utils_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
