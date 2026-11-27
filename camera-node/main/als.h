#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * LTR-308ALS-01 ambient light sensor, I2C address 0x53, on the camera's SCCB bus
 * (GPIO8/GPIO9, confirmed on the schematic).
 */

/* Attach to the camera's I2C bus, log the devices found, configure the sensor
 * (16 bit / 25 ms, gain 3x) and put it in standby. Call AFTER camsvc_init(). */
esp_err_t als_init(void);
bool als_available(void);

/* Start measuring (standby -> active). First sample is ready ~30 ms later. */
esp_err_t als_wake(void);

/* Stop measuring (about 1 uA). */
void als_standby(void);

/* Wait up to timeout_ms for a fresh sample and return it in lux. */
esp_err_t als_read_lux(float *lux, uint32_t timeout_ms);

/* Log scale 0..100 (0 = dark, 100 = ~50 000 lux), the unit of the light threshold. */
int als_lux_to_level(float lux);
