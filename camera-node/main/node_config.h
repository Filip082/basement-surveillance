#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ====================================================================
 * HARD-CODED DEFAULTS (step 1 of the flow)
 * Later these will be written by a BLE setup procedure; until then edit here.
 * ==================================================================== */
#define NODE_NUM_FRAME_SIZES        6   /* 0 QVGA, 1 VGA, 2 SVGA, 3 XGA, 4 SXGA, 5 UXGA */
#define NODE_DEFAULT_FRAME_SIZE_IDX 2   /* SVGA 800x600 */
#define NODE_DEFAULT_JPEG_QUALITY   12  /* 8 (best) .. 63 (smallest file) */
#define NODE_DEFAULT_LIGHT_THR      30  /* 0 (dark) .. 100 (bright), log scale; 30 ~ 26 lux */

/* Frames thrown away after waking the sensor so auto exposure can settle. */
#define NODE_SETTLE_FRAMES_DAY      3
#define NODE_SETTLE_FRAMES_NIGHT    6   /* with IR light: AEC needs longer */

/* If the light sensor cannot be read: 1 = assume dark (IR on), 0 = assume bright. */
#define NODE_IR_WHEN_LIGHT_UNKNOWN  1

/* 1 = at boot, if nothing is stored in flash yet, write the defaults above to
 *     flash and mark the node "configured" (use to test the persistence path).
 * 0 = flash stays untouched until node_config_save() is called by setup code. */
#define NODE_STORE_DEFAULTS_TO_FLASH 0

/* "Factory reset" gesture: after a reset, press and hold BOOT for HOLD ms within WINDOW ms.
 * Erases pairings + stored config and opens the pairing window. WINDOW = 0 disables it
 * (and removes the wait at start-up). */
#define NODE_RESET_BUTTON_WINDOW_MS 1500
#define NODE_RESET_BUTTON_HOLD_MS   1000

/* Settings that define how the node behaves. */
typedef struct {
    uint8_t frame_size_idx;     /* index 0..NODE_NUM_FRAME_SIZES-1 */
    uint8_t jpeg_quality;       /* 8..63 */
    uint8_t light_threshold;    /* 0..100: below this level it is "night" -> IR on */
} node_config_t;

/* Load settings: flash copy if the "configured" flag is set and valid, otherwise defaults. */
void node_config_load(void);

/* Settings in use (never NULL). */
const node_config_t *node_config_get(void);

/* true if the settings in use came from flash, false if they are the hard-coded defaults. */
bool node_config_is_from_flash(void);

/* Store settings in flash, set the "configured" flag and use them from now on. */
esp_err_t node_config_save(const node_config_t *cfg);

/* Remove the flash copy and fall back to the defaults (the "factory reset"). */
esp_err_t node_config_erase(void);
