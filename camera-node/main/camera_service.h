#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_camera.h"
#include "node_config.h"

/*
 * OV3660 camera service. The camera is initialised ONCE (camsvc_init), then kept
 * in sensor software-standby between captures so a capture needs no re-init.
 */

/* Full initialisation with the given settings; leaves the sensor in standby. */
esp_err_t camsvc_init(const node_config_t *cfg);

/* Deinit + init again (recovery path, or after the settings changed). */
esp_err_t camsvc_reinit(const node_config_t *cfg);

/* Sensor into software power-down (registers are kept). */
void camsvc_standby(void);

/* Sensor out of power-down. Falls back to a full init if the camera was never initialised. */
esp_err_t camsvc_wake(void);

/*
 * Throw away settle_frames frames (auto-exposure convergence + stale frames from
 * before standby), then return the next frame, checked to be a complete JPEG.
 * Recovers by re-initialising the camera once if frames stop arriving.
 * Give the frame back with camsvc_release(). NULL on failure.
 */
camera_fb_t *camsvc_capture(int settle_frames);
void camsvc_release(camera_fb_t *fb);

/* Human readable name of a frame size index, e.g. "SVGA 800x600". */
const char *camsvc_size_name(uint8_t idx);
