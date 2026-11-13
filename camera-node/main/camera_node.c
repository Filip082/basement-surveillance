/*
 * camera-node: battery powered camera for the DFRobot DFR1154 (ESP32-S3 AI CAM).
 *
 *   boot (once)                         event cycle (every wake-up)
 *   1  config (hard-coded / flash)      6  wake-up event detected
 *   2  start camera                     7  check ambient light
 *   3  start BLE (self-test)            8  dark -> IR LEDs on
 *   4  start light sensor               9  capture one frame
 *   5  light sleep until GPIO event     10 IR LEDs off
 *                                       11 BLE: advertise, wait for the paired computer
 *                                       12 send the frame (ACK from the computer)
 *                                       13 power everything down
 *                                       14 light sleep until GPIO event  -> back to 6
 *
 * Light sleep keeps RAM and CPU state, so nothing is re-initialised between events.
 * See README.md for the details of every module.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "als.h"
#include "ble_link.h"
#include "board_io.h"
#include "camera_service.h"
#include "node_config.h"
#include "node_log.h"
#include "power.h"

#define T "node"

/* Step 7: measure the light and decide whether the IR LEDs are needed. */
static bool is_dark(const node_config_t *cfg)
{
    float lux = 0;
    esp_err_t err = als_read_lux(&lux, 200);
    if (err != ESP_OK) {
        NSTEP(7, "light sensor unavailable (%s) -> assuming %s", esp_err_to_name(err),
              NODE_IR_WHEN_LIGHT_UNKNOWN ? "dark" : "bright");
        return NODE_IR_WHEN_LIGHT_UNKNOWN;
    }
    int level = als_lux_to_level(lux);
    bool dark = level < cfg->light_threshold;
    NSTEP(7, "light %d lux = level %d, threshold %u -> %s", (int)(lux + 0.5f), level,
          cfg->light_threshold, dark ? "DARK" : "bright");
    return dark;
}

/*
 * One complete event cycle (steps 6-13). pairing_window = true is used right after
 * power-up when no computer is paired yet: it waits longer for a computer to appear.
 */
static void run_cycle(bool pairing_window)
{
    const node_config_t *cfg = node_config_get();
    node_log_cycle_start();
    NSTEP(6, "event detected%s", pairing_window ? " (pairing cycle after power-up)" : "");

    /* Wake the camera and the light sensor together; the sensor needs ~30 ms anyway. */
    if (camsvc_wake() != ESP_OK) {
        NLOGE(T, "camera could not be woken");
    }
    als_wake();

    bool dark = is_dark(cfg);                                       /* 7 */

    if (dark) {                                                     /* 8 */
        board_ir_set(true);
        NSTEP(8, "dark -> IR LEDs ON");
    } else {
        NSTEP(8, "bright enough -> IR LEDs stay off");
    }

    camera_fb_t *fb = camsvc_capture(dark ? NODE_SETTLE_FRAMES_NIGHT
                                          : NODE_SETTLE_FRAMES_DAY); /* 9 */
    if (fb) {
        NSTEP(9, "frame captured: %ux%u, %u bytes", (unsigned)fb->width,
              (unsigned)fb->height, (unsigned)fb->len);
    } else {
        NSTEP(9, "capture FAILED");
    }

    board_ir_set(false);                                            /* 10 */
    NSTEP(10, "IR LEDs OFF");

    /* Camera and light sensor are not needed any more: power them down before the
     * (long, radio-heavy) BLE part. The captured frame stays in PSRAM. */
    camsvc_standby();
    als_standby();

    if (fb) {
        uint32_t wait_ms = pairing_window ? CONFIG_NODE_BLE_PAIRING_WINDOW_S * 1000u
                                          : CONFIG_NODE_BLE_CONNECT_TIMEOUT_MS;
        if (ble_link_start(true) == ESP_OK) {                       /* 11 */
            NSTEP(11, "BLE started, advertising; waiting up to %u ms for the computer",
                  (unsigned)wait_ms);

            if (ble_link_wait_ready(wait_ms) == ESP_OK) {
                NSTEP(11, "computer connected, paired and listening");

                for (int attempt = 1; attempt <= 2; attempt++) {    /* 12 */
                    if (ble_link_send_image(fb->buf, fb->len, fb->width, fb->height) != ESP_OK) {
                        NSTEP(12, "sending FAILED (attempt %d)", attempt);
                        break;
                    }
                    NSTEP(12, "frame sent (attempt %d), waiting for ACK", attempt);
                    ble_ack_t ack = ble_link_wait_ack(CONFIG_NODE_BLE_ACK_TIMEOUT_MS);
                    if (ack == BLE_ACK_OK) {
                        NSTEP(12, "ACK received - delivered");
                        break;
                    } else if (ack == BLE_ACK_NAK) {
                        NSTEP(12, "computer asked for a resend");
                    } else {
                        NSTEP(12, "no ACK within %d ms (receiver may not send one) - assuming delivered",
                              CONFIG_NODE_BLE_ACK_TIMEOUT_MS);
                        break;
                    }
                }
            } else {
                NSTEP(11, "no computer -> frame dropped");
            }
        } else {
            NSTEP(11, "BLE failed to start -> frame dropped");
        }
        ble_link_stop();
        camsvc_release(fb);
    }

    NSTEP(13, "BLE off, IR off, camera + light sensor in standby");
    NLOGI(T, "cycle finished in %lld ms", (long long)node_log_elapsed_ms());
}

void app_main(void)
{
#if !LOGS_ENABLED
    esp_log_level_set("*", ESP_LOG_NONE);
#endif
    node_log_cycle_start();
    NLOGI(T, "==== camera node start ====");

    /* NVS holds the stored config and the BLE pairing keys. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Factory-reset gesture: erase pairings + stored config. */
    bool factory_reset = false;
#if NODE_RESET_BUTTON_WINDOW_MS > 0
    NLOGI(T, "hold BOOT for %d ms now to factory-reset (pairings + config)", NODE_RESET_BUTTON_HOLD_MS);
    factory_reset = board_boot_button_held(NODE_RESET_BUTTON_WINDOW_MS, NODE_RESET_BUTTON_HOLD_MS);
    if (factory_reset) {
        NLOGW(T, "FACTORY RESET requested");
        ble_link_erase_bonds();
        node_config_erase();
    }
#endif

    /* 1: configuration */
    node_config_load();
    const node_config_t *cfg = node_config_get();
    NSTEP(1, "configured: %s, JPEG quality %u, light threshold %u (%s)",
          camsvc_size_name(cfg->frame_size_idx), cfg->jpeg_quality, cfg->light_threshold,
          node_config_is_from_flash() ? "from flash" : "hard-coded");

    /* 2: camera (stays initialised, parked in standby) */
    err = camsvc_init(cfg);
    NSTEP(2, "camera %s", err == ESP_OK ? "started and in standby" : "FAILED to start (will retry on capture)");

    /* 3: BLE self-test: bring the stack up once, read the pairing state, shut it down again */
    int bonds = 0;
    if (ble_link_start(false) == ESP_OK) {
        if (ble_link_wait_synced(3000) == ESP_OK) {
            bonds = ble_link_bond_count();
            NSTEP(3, "BLE works, %d paired computer(s)", bonds);
        } else {
            NSTEP(3, "BLE stack did not become ready");
        }
        ble_link_stop();
    } else {
        NSTEP(3, "BLE FAILED to start");
    }

    /* 4: light sensor (shares the camera's I2C bus, so after the camera) */
    err = als_init();
    NSTEP(4, "light sensor %s", err == ESP_OK ? "ready" : "NOT available");

    power_wake_gpio_init();

    /* First use: nobody paired yet -> stay reachable so a computer can pair. */
    if (factory_reset || bonds == 0) {
        NLOGI(T, "no paired computer: running a pairing cycle now");
        run_cycle(true);
    }

    /* 5 / 14 and 6..13: sleep, wake on the GPIO event, run one cycle, repeat */
    bool first = true;
    for (;;) {
        NSTEP(first ? 5 : 14, "sleeping until the wake-up line goes %s",
#ifdef CONFIG_NODE_WAKE_ACTIVE_LOW
              "LOW"
#else
              "HIGH"
#endif
              );
        first = false;
        if (power_sleep_until_event()) {
            run_cycle(false);
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));     /* sleep failed: do not spin */
        }
    }
}
