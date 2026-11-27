#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t percent;        /* 0..100, linear between NODE_BATTERY_MIN_MV and NODE_BATTERY_MAX_MV */
    uint16_t battery_mv;    /* battery voltage (ADC pin voltage x divider ratio) */
    uint16_t adc_mv;        /* voltage measured at the ADC pin */
} battery_reading_t;

/*
 * Measure the battery. The ADC is switched on only for the duration of this call:
 * unit + channel are created, NODE_BATTERY_SAMPLES conversions are averaged (calibrated, mV),
 * then everything is deleted again. Returns ESP_OK or an error (e.g. the configured GPIO has
 * no ADC channel).
 */
esp_err_t battery_measure(battery_reading_t *out);
