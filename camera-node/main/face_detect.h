#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int faces;              /* number of faces found */
    float best_score;       /* confidence of the best face (0..1) */
    uint8_t *jpeg;          /* picture with frames drawn in (only if faces > 0 and draw_frames).
                               Allocated with malloc: release with free(). NULL if framing failed. */
    size_t jpeg_len;
} face_result_t;

/* Load the detection model. Call once (slow, ~100+ ms). */
esp_err_t face_detect_init(void);

/*
 * Look for faces in a JPEG.
 *   1. decode the JPEG to RGB888 (PSRAM)
 *   2. run the detector (MSR + MNP)
 *   3. if faces were found and draw_frames: draw a rectangle (colour, thickness from
 *      Kconfig) around each face and encode the result to a new JPEG
 * Returns ESP_OK also when no face was found (res->faces == 0).
 */
esp_err_t face_detect_run(const uint8_t *jpeg, size_t jpeg_len, bool draw_frames,
                          face_result_t *res);

#ifdef __cplusplus
}
#endif
