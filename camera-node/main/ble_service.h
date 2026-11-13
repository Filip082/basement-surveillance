#pragma once

#include "esp_err.h"

/*
 * Starts NimBLE, registers the camera GATT service and begins advertising.
 * Call after nvs_flash_init().
 *
 * GATT service  dfa10001-5b3c-4f6d-9e21-0a4c7b8d1e00
 *   Control  dfa10002-...  Write / Write-no-rsp   command: [id][arg]
 *   Image    dfa10003-...  Notify                 JPEG stream (see README)
 *   Status   dfa10004-...  Read                   text status line
 */
esp_err_t ble_service_start(void);
