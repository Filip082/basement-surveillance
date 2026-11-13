#include "board_io.h"

#include <stdint.h>
#include "driver/gpio.h"
#include "board_dfr1154.h"

static uint64_t s_configured;
static bool s_ir_on;

static void set_pin(int pin, int level)
{
    if (!(s_configured & (1ULL << pin))) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        s_configured |= 1ULL << pin;
    }
    gpio_set_level(pin, level);
}

void board_led_set(bool on)
{
    set_pin(BOARD_PIN_LED, on ? 1 : 0);
}

void board_ir_set(bool on)
{
    s_ir_on = on;
    set_pin(BOARD_PIN_IR, on ? 1 : 0);
}

bool board_ir_get(void)
{
    return s_ir_on;
}
