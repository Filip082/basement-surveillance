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

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "als.h"
#include "ble_link.h"
#include "battery.h"
#include "board_io.h"
#include "camera_service.h"
#include "vision.h"
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

/* The picture that goes out over BLE, and what has to be released afterwards. */
typedef struct {
    const uint8_t *buf;
    size_t len;
    uint16_t width, height;
    uint8_t flags;          /* BLE_IMG_FLAG_* */
    uint8_t faces;
    uint8_t humans;
    camera_fb_t *fb;        /* camera frame to give back (or NULL) */
    uint8_t *owned;         /* malloc'ed buffer to free (picture with frames), or NULL */
} out_image_t;

static void out_release(out_image_t *o)
{
    if (o->fb) {
        camsvc_release(o->fb);
    }
    free(o->owned);
    memset(o, 0, sizeof(*o));
}

static void out_from_fb(out_image_t *o, camera_fb_t *fb)
{
    o->buf = fb->buf;
    o->len = fb->len;
    o->width = fb->width;
    o->height = fb->height;
    o->fb = fb;
}

/* Steps 9a-9c: take ONE picture, no face search. */
static bool capture_single(bool dark, out_image_t *out)
{
    camera_fb_t *fb = camsvc_capture(dark ? NODE_SETTLE_FRAMES_NIGHT : NODE_SETTLE_FRAMES_DAY);
    if (!fb) {
        NSTEP(9, "capture FAILED");
        return false;
    }
    NSTEP(9, "frame captured: %ux%u, %u bytes", (unsigned)fb->width, (unsigned)fb->height,
          (unsigned)fb->len);
    out_from_fb(out, fb);
    return true;
}

#ifdef CONFIG_NODE_FACE_DETECT
/* Copy a camera frame into our own buffer so the frame can be given back to the driver. */
static bool out_copy_fb(out_image_t *o, const camera_fb_t *fb)
{
    uint8_t *copy = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
    if (!copy) {
        return false;
    }
    memcpy(copy, fb->buf, fb->len);
    o->owned = copy;
    o->buf = copy;
    o->len = fb->len;
    o->width = fb->width;
    o->height = fb->height;
    return true;
}

/*
 * Steps 9a-9c with person / face search. Repeats until NODE_FACE_WINDOW_MS after the
 * wake-up, with NODE_FACE_RETRY_INTERVAL_MS between the starts of two pictures:
 *
 *   person?  no            -> next picture
 *            yes, face?    -> yes: STOP, send this picture ("face detected")
 *                             no:  remember this picture ("human"), next picture
 *
 * When the time is over: the remembered person picture goes out as "human detected",
 * or - if no person was ever seen - the last picture as "no human detected".
 * (With NODE_PERSON_DETECT off, only faces are searched: face / none.)
 */
static bool capture_smart(bool dark, int64_t t_wake_us, out_image_t *out)
{
    out_image_t saved = {0};        /* best picture so far that contains a person */

    for (int shot = 1;; shot++) {
        int64_t t_shot_us = esp_timer_get_time();
        camera_fb_t *fb = camsvc_capture(shot == 1 ? (dark ? NODE_SETTLE_FRAMES_NIGHT
                                                           : NODE_SETTLE_FRAMES_DAY)
                                                   : CONFIG_NODE_FACE_RETRY_SETTLE_FRAMES);
        if (!fb) {
            NSTEP(9, "shot %d: capture FAILED", shot);
            if (saved.buf) {                          /* still have a person picture */
                *out = saved;
                return true;
            }
            return false;
        }

        vision_result_t vr;
        esp_err_t err = vision_analyze(fb->buf, fb->len, &vr);
        int64_t now_us = esp_timer_get_time();
        NSTEP(9, "shot %d: %ux%u, %u bytes, %d person(s), %d face(s) (analysis %lld ms, %lld ms since wake-up)",
              shot, (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len,
              err == ESP_OK ? vr.humans : -1, err == ESP_OK ? vr.faces : -1,
              (long long)((now_us - t_shot_us) / 1000), (long long)((now_us - t_wake_us) / 1000));

        if (err != ESP_OK) {                          /* analysis broken: send as is */
            if (saved.buf) {
                *out = saved;
                camsvc_release(fb);
            } else {
                out_from_fb(out, fb);
            }
            return true;
        }

        if (vr.faces > 0) {                           /* 3: face -> done */
            out_release(&saved);
            if (vr.jpeg) {
                out->owned = vr.jpeg;
                out->buf = vr.jpeg;
                out->len = vr.jpeg_len;
                out->width = fb->width;
                out->height = fb->height;
                camsvc_release(fb);
            } else {
                out_from_fb(out, fb);
            }
            out->flags = BLE_IMG_FLAG_FACE | BLE_IMG_FLAG_HUMAN;
            out->faces = (uint8_t)(vr.faces > 255 ? 255 : vr.faces);
            out->humans = (uint8_t)(vr.humans > 255 ? 255 : vr.humans);
            return true;
        }

        if (vr.humans > 0) {                          /* 2: person without face -> remember it */
            out_release(&saved);
            if (vr.jpeg) {
                saved.owned = vr.jpeg;
                saved.buf = vr.jpeg;
                saved.len = vr.jpeg_len;
                saved.width = fb->width;
                saved.height = fb->height;
            } else if (!out_copy_fb(&saved, fb)) {
                NLOGW(T, "no memory to keep the picture");
            }
            saved.flags = BLE_IMG_FLAG_HUMAN;
            saved.humans = (uint8_t)(vr.humans > 255 ? 255 : vr.humans);
        } else {
            free(vr.jpeg);
        }

        if ((now_us - t_wake_us) / 1000 >= CONFIG_NODE_FACE_WINDOW_MS) {
            if (saved.buf) {
                NSTEP(9, "no face within %d ms - sending the picture with a person", CONFIG_NODE_FACE_WINDOW_MS);
                *out = saved;
                camsvc_release(fb);
            } else {
                NSTEP(9, "nobody within %d ms - sending the last picture", CONFIG_NODE_FACE_WINDOW_MS);
                out_from_fb(out, fb);
            }
            return true;
        }

        camsvc_release(fb);
        int64_t wait_us = t_shot_us + (int64_t)CONFIG_NODE_FACE_RETRY_INTERVAL_MS * 1000
                          - esp_timer_get_time();
        if (wait_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
        }
    }
}
#endif

/*
 * One complete event cycle (steps 6-13). pairing_window = true is used right after
 * power-up when no computer is paired yet: it waits longer for a computer to appear.
 */
static void run_cycle(bool pairing_window)
{
    const node_config_t *cfg = node_config_get();
    node_log_cycle_start();
    const int64_t t_wake_us = esp_timer_get_time();
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

    out_image_t out = {0};                                          /* 9 */
#ifdef CONFIG_NODE_FACE_DETECT
    bool have = capture_smart(dark, t_wake_us, &out);
#else
    (void)t_wake_us;
    bool have = capture_single(dark, &out);
#endif

    board_ir_set(false);                                            /* 10 */
    NSTEP(10, "IR LEDs OFF");

    /* Camera and light sensor are not needed any more: power them down before the
     * (long, radio-heavy) BLE part. The picture stays in PSRAM. */
    camsvc_standby();
    als_standby();

    if (have) {
        uint32_t wait_ms = pairing_window ? CONFIG_NODE_BLE_PAIRING_WINDOW_S * 1000u
                                          : CONFIG_NODE_BLE_CONNECT_TIMEOUT_MS;
        if (ble_link_start(true) == ESP_OK) {                       /* 11 */
            NSTEP(11, "BLE started, advertising; waiting up to %u ms for the computer",
                  (unsigned)wait_ms);

            if (ble_link_wait_ready(wait_ms) == ESP_OK) {
                NSTEP(11, "computer connected, paired and listening");

                for (int attempt = 1; attempt <= 2; attempt++) {    /* 12 */
                    if (ble_link_send_image(out.buf, out.len, out.width, out.height,
                                            out.flags, out.faces, out.humans) != ESP_OK) {
                        NSTEP(12, "sending FAILED (attempt %d)", attempt);
                        break;
                    }
                    NSTEP(12, "frame sent (attempt %d, %s), waiting for ACK", attempt,
                          (out.flags & BLE_IMG_FLAG_FACE) ? "FACE DETECTED"
                          : (out.flags & BLE_IMG_FLAG_HUMAN) ? "HUMAN DETECTED" : "no human detected");
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
        out_release(&out);
    }

    NSTEP(13, "BLE off, IR off, camera + light sensor in standby");
    NLOGI(T, "cycle finished in %lld ms", (long long)node_log_elapsed_ms());
}

/*
 * The wake-up line went active. A SHORT pulse (released within NODE_BATTERY_REQUEST_MS)
 * asks for a picture, a LONG one (still active after that time) asks for the battery level.
 * Returns true for a battery request. Polls only for at most that time.
 */
static bool wake_is_battery_request(void)
{
    const int64_t t0 = esp_timer_get_time();
    while (power_wake_pin_active()) {
        if ((esp_timer_get_time() - t0) / 1000 >= CONFIG_NODE_BATTERY_REQUEST_MS) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

/*
 * Battery cycle: measure (ADC on only for the measurement), start BLE, send the battery
 * report, wait for the ACK, BLE off. The camera is not touched at all.
 */
static void run_battery_cycle(void)
{
    node_log_cycle_start();
    NSTEP(6, "BATTERY request (wake line LOW for more than %d ms)", CONFIG_NODE_BATTERY_REQUEST_MS);

    battery_reading_t bat;
    if (battery_measure(&bat) != ESP_OK) {                          /* 7: ADC on -> sample -> off */
        NSTEP(7, "battery measurement FAILED - nothing sent");
        return;
    }
    NSTEP(7, "battery %u %% (%u mV, %u mV at the pin)", bat.percent, bat.battery_mv, bat.adc_mv);

    if (ble_link_start(true) == ESP_OK) {                           /* 11 */
        NSTEP(11, "BLE started, advertising; waiting up to %d ms for the computer",
              CONFIG_NODE_BLE_CONNECT_TIMEOUT_MS);
        if (ble_link_wait_ready(CONFIG_NODE_BLE_CONNECT_TIMEOUT_MS) == ESP_OK) {
            if (ble_link_send_battery(bat.percent, bat.battery_mv, bat.adc_mv) == ESP_OK) {   /* 12 */
                NSTEP(12, "battery report sent, waiting for ACK");
                ble_ack_t ack = ble_link_wait_ack(CONFIG_NODE_BLE_ACK_TIMEOUT_MS);
                NSTEP(12, ack == BLE_ACK_OK ? "ACK received - delivered" : "no ACK - assuming delivered");
            } else {
                NSTEP(12, "sending the battery report FAILED");
            }
        } else {
            NSTEP(11, "no computer -> report dropped");
        }
    } else {
        NSTEP(11, "BLE failed to start -> report dropped");
    }
    ble_link_stop();
    NSTEP(13, "BLE off");
    NLOGI(T, "battery cycle finished in %lld ms", (long long)node_log_elapsed_ms());
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

#ifdef CONFIG_NODE_FACE_DETECT
    /* face detection model (loaded once; ~1.5 MB read from flash) */
    err = vision_init();
    NLOGI(T, "person/face detectors %s", err == ESP_OK ? "ready" : "FAILED to load (pictures go out unchecked)");
#endif

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
            if (wake_is_battery_request()) {
                run_battery_cycle();
            } else {
                run_cycle(false);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));     /* sleep failed: do not spin */
        }
    }
}
