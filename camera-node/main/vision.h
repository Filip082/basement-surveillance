#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int humans;             /* number of people found (0 if person detection is off) */
    int faces;              /* number of faces found */
    float human_score;      /* best confidence, 0..1 */
    float face_score;
    uint8_t *jpeg;          /* picture with frames drawn in, only if something was framed.
                               Allocated with malloc: release with free(). NULL otherwise
                               (nothing to draw, or re-encoding failed). */
    size_t jpeg_len;
} vision_result_t;

/* Load the models (person + face). Call once; slow (~100+ ms per model). */
esp_err_t vision_init(void);

/*
 * Analyse one JPEG:
 *   1. decode it to RGB888 (PSRAM)
 *   2. person detector (if NODE_PERSON_DETECT)
 *   3. face detector - only when a person was found, unless NODE_FACE_WITHOUT_PERSON
 *      (or person detection is off: then it always runs)
 *   4. face found -> frame the faces (NODE_FACE_BOX_COLOR); else person found -> frame the
 *      people (NODE_PERSON_BOX_COLOR) if NODE_PERSON_DRAW_BOX; re-encode into res->jpeg
 * Returns ESP_OK also when nothing was found.
 */
esp_err_t vision_analyze(const uint8_t *jpeg, size_t jpeg_len, vision_result_t *res);

#ifdef __cplusplus
}
#endif
