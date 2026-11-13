#include "node_log.h"

#include <stdarg.h>
#include <stdio.h>
#include "esp_timer.h"

static int64_t s_t0_us;

void node_log_cycle_start(void)
{
    s_t0_us = esp_timer_get_time();
}

int64_t node_log_elapsed_ms(void)
{
    return (esp_timer_get_time() - s_t0_us) / 1000;
}

void node_log_step(int num, const char *fmt, ...)
{
#if LOGS_ENABLED
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGI("step", "[STEP %2d] %s (+%lld ms)", num, msg, (long long)node_log_elapsed_ms());
#else
    (void)num;
    (void)fmt;
#endif
}
