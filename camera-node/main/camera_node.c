/*
 * camera-node: camera + BLE demo for the DFRobot DFR1154 (ESP32-S3 AI CAM).
 *
 *  1. initialises the OV3660 and runs a serial self-test (3 JPEG frames)
 *  2. starts a BLE GATT server that lets a phone/PC trigger captures and
 *     receive the JPEG over notifications
 */

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "camera_service.h"
#include "ble_service.h"
#include "als.h"
#include "daynight.h"

static const char *TAG = "camera_node";

void app_main(void)
{
    /* NVS is required by the BLE/PHY stack */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM: %u bytes %s", (unsigned)psram,
             psram ? "" : "(NOT FOUND - check CONFIG_SPIRAM_MODE_OCT)");

    /* BLE first: the Bluetooth controller needs a big block of internal RAM,
     * so it is started before the camera driver allocates its DMA buffers. */
    ESP_LOGI(TAG, "starting BLE");
    ESP_ERROR_CHECK(ble_service_start());

    /* Camera problems must not stop BLE: the status characteristic reports them. */
    ESP_LOGI(TAG, "starting camera");
    if (camsvc_init() == ESP_OK) {
        camsvc_self_test();
    } else {
        ESP_LOGE(TAG, "camera unavailable; continuing with BLE only");
    }

    als_init();      /* after the camera: it attaches to the camera's I2C bus */
    daynight_start();
    ESP_LOGI(TAG, "ready - connect with tools/ble_viewer.html or nRF Connect");
}
