#pragma once

#include <stdbool.h>

/* On-board LED (GPIO3) and IR illumination (GPIO47). Pins are configured on first use. */
void board_led_set(bool on);
void board_ir_set(bool on);
bool board_ir_get(void);
