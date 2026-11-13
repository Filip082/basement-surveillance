#include "board_io.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_dfr1154.h"

/*
 * NOTE: per-pin gpio_* calls are used on purpose instead of gpio_config() with a
 * pin mask. On this board/IDF combination a gpio_config() call that included
 * GPIO47 ended in a silent watchdog reset during development.
 */

static bool s_ir_configured;
static bool s_ir_on;

static void ir_configure(void)
{
    if (!s_ir_configured) {
        gpio_reset_pin(BOARD_PIN_IR);
        gpio_set_level(BOARD_PIN_IR, 0);
        gpio_set_direction(BOARD_PIN_IR, GPIO_MODE_OUTPUT);
        s_ir_configured = true;
    }
}

void board_ir_set(bool on)
{
    ir_configure();
    gpio_hold_dis(BOARD_PIN_IR);
    gpio_set_level(BOARD_PIN_IR, on ? 1 : 0);
    s_ir_on = on;
}

bool board_ir_get(void)
{
    return s_ir_on;
}

void board_prepare_sleep(void)
{
    board_ir_set(false);
    gpio_hold_en(BOARD_PIN_IR);         /* keep it low while the chip sleeps */
}

void board_resume(void)
{
    if (s_ir_configured) {
        gpio_hold_dis(BOARD_PIN_IR);
    }
}

bool board_boot_button_held(unsigned window_ms, unsigned hold_ms)
{
    gpio_reset_pin(BOARD_PIN_BOOT);
    gpio_set_direction(BOARD_PIN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_PIN_BOOT, GPIO_PULLUP_ONLY);

    unsigned low_ms = 0;
    for (unsigned t = 0; t < window_ms; t += 20) {
        if (gpio_get_level(BOARD_PIN_BOOT) == 0) {
            low_ms += 20;
            if (low_ms >= hold_ms) {
                return true;
            }
        } else {
            low_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}
