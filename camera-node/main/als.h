#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * LTR-308 ambient light sensor (I2C address 0x53) on the DFR1154.
 * It is assumed to share the camera's SCCB bus (GPIO8/GPIO9), so call this
 * AFTER the camera is initialised: it attaches to the camera's I2C bus and
 * logs every device found on it.
 */
esp_err_t als_init(void);
bool als_available(void);

/* Ambient light in lux. */
esp_err_t als_read_lux(float *lux);

/* Log-scale 0..100 (0 = dark, 100 = ~50 000 lux) used by the day/night slider. */
int als_lux_to_level(float lux);
