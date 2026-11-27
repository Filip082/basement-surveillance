#include "node_log.h"

#include <stdarg.h>
#include <stdio.h>
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_io.h"

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
#ifdef CONFIG_NODE_DEBUG_LED_STEPS
    /* Progress on the LED for when the serial port is not usable (light sleep):
     * steps 6..13 blink (num-5) times, step 14 / 5 (going to sleep) = one long blink. */
    if (num >= 6 && num <= 13) {
        vTaskDelay(pdMS_TO_TICKS(500));
        for (int i = 0; i < num - 5; i++) {
            board_led_blink(80);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    } else if (num == 14) {
        vTaskDelay(pdMS_TO_TICKS(500));
        board_led_blink(600);
    }
#endif
#else
    (void)num;
    (void)fmt;
#endif
}
