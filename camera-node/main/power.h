#pragma once

#include <stdbool.h>

/* Configure the wake-up input (Kconfig NODE_WAKE_GPIO, default GPIO10) with the right pull. */
void power_wake_gpio_init(void);

/* true while the wake-up line is at its active level. */
bool power_wake_pin_active(void);

/*
 * Enter light sleep and block until the wake-up line becomes active (step 5 / 14).
 * Returns true when the caller should run a capture cycle.
 * Everything keeps its RAM state: execution simply continues after this call.
 */
bool power_sleep_until_event(void);
