#include "power.h"

#include <stdio.h>
#include "driver/gpio.h"
#include "esp_rom_serial_output.h"
#include "esp_bit_defs.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

#include "board_io.h"
#include "node_log.h"

#define PIN ((gpio_num_t)CONFIG_NODE_WAKE_GPIO)

#ifdef CONFIG_NODE_WAKE_ACTIVE_LOW
#define ACTIVE_LEVEL   0
#define WAKE_LEVEL     GPIO_INTR_LOW_LEVEL
#define PULL_MODE      GPIO_PULLUP_ONLY       /* idle high, event pulls it low */
#else
#define ACTIVE_LEVEL   1
#define WAKE_LEVEL     GPIO_INTR_HIGH_LEVEL
#define PULL_MODE      GPIO_PULLDOWN_ONLY     /* idle low, event drives it high */
#endif

static void flush_console(void)
{
    fflush(stdout);
#ifdef CONFIG_ESP_CONSOLE_UART_NUM
    esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
}

void power_wake_gpio_init(void)
{
    gpio_reset_pin(PIN);
    gpio_set_direction(PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN, PULL_MODE);
    /* keep the same pull while the chip is in light sleep */
    gpio_sleep_set_direction(PIN, GPIO_MODE_INPUT);
    gpio_sleep_set_pull_mode(PIN, PULL_MODE);
    NLOGI("power", "wake input GPIO%d, active %s, internal pull-%s",
          CONFIG_NODE_WAKE_GPIO, ACTIVE_LEVEL ? "HIGH" : "LOW", ACTIVE_LEVEL ? "down" : "up");
}

bool power_wake_pin_active(void)
{
    return gpio_get_level(PIN) == ACTIVE_LEVEL;
}

bool power_sleep_until_event(void)
{
    /* Light-sleep wake-up is level triggered: if the line is still active
     * (e.g. a PIR holding its output) we would wake again immediately. Wait for
     * it to be released, polling with short timer sleeps to stay low power. */
    bool warned = false;
    while (power_wake_pin_active()) {
        if (!warned) {
            NLOGW("power", "wake line still active - waiting for it to be released");
            warned = true;
        }
        esp_sleep_enable_timer_wakeup(200 * 1000);
        flush_console();
        esp_light_sleep_start();
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    gpio_wakeup_enable(PIN, WAKE_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    board_prepare_sleep();

    NLOGI("power", "entering light sleep, waiting for GPIO%d", CONFIG_NODE_WAKE_GPIO);
    flush_console();

    esp_err_t err = esp_light_sleep_start();

    board_resume();
    gpio_wakeup_disable(PIN);

    if (err == ESP_OK) {
        uint32_t causes = esp_sleep_get_wakeup_causes();   /* bit mask: BIT(cause) */
        bool gpio = (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) != 0;
        NLOGI("power", "woke up, causes=0x%x (%s)", (unsigned)causes, gpio ? "GPIO" : "other");
        return gpio;
    }
    if (err == ESP_ERR_SLEEP_REJECT) {
        /* the line became active between the check above and the sleep call */
        NLOGW("power", "sleep rejected: event arrived just before sleeping");
        return true;
    }
    NLOGE("power", "light sleep failed: 0x%x", err);
    return false;
}
