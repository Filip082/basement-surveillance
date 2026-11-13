#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_camera.h"

/* Initialise the OV3660 and the DVP interface. */
esp_err_t camsvc_init(void);

/* Grab 3 frames, log size / JPEG sanity / timing. Returns ESP_OK if all frames were valid JPEGs. */
esp_err_t camsvc_self_test(void);

/* Blocking capture of the most recent frame. Release with camsvc_release(). NULL on failure. */
camera_fb_t *camsvc_capture(void);
void camsvc_release(camera_fb_t *fb);

/* Runtime settings. Size index: 0=QVGA 1=VGA 2=SVGA 3=XGA 4=SXGA 5=UXGA */
esp_err_t camsvc_set_size_index(uint8_t idx);
esp_err_t camsvc_set_quality(uint8_t q);

/* Human readable one-line status (sensor, size, counters, free PSRAM). */
void camsvc_status_string(char *buf, size_t len);

/*
 * Ambient light estimate from the sensor's auto-exposure (exposure x gain, log scale).
 * light_pct: 0 = very dark .. 100 = very bright. Also returns the raw register values.
 */
esp_err_t camsvc_read_light(int *light_pct, uint32_t *exp_raw, uint32_t *gain_raw);
