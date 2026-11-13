#include "camera_service.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sensor.h"
#include "sdkconfig.h"
#include "board_dfr1154.h"

static const char *TAG = "cam";

static const framesize_t k_sizes[] = {
    FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA,
    FRAMESIZE_XGA,  FRAMESIZE_SXGA, FRAMESIZE_UXGA,
};
#define N_SIZES (sizeof(k_sizes) / sizeof(k_sizes[0]))

static SemaphoreHandle_t s_lock;     /* serialises sensor register access */

static struct {
    bool     ok;
    esp_err_t init_err;
    uint16_t pid;
    uint8_t  size_idx;
    uint8_t  quality;
    uint32_t frames;
    uint32_t last_len;
    uint16_t last_w, last_h;
} s = { .quality = CONFIG_CAMNODE_JPEG_QUALITY };

/* After a settings change, frames already queued still have the old settings. */
static void flush_stale_frames(void)
{
    for (int i = 0; i < 3; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
}

esp_err_t camsvc_init(void)
{
    const camera_config_t cfg = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk  = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d0 = CAM_PIN_D0,
        .pin_d1 = CAM_PIN_D1,
        .pin_d2 = CAM_PIN_D2,
        .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4,
        .pin_d5 = CAM_PIN_D5,
        .pin_d6 = CAM_PIN_D6,
        .pin_d7 = CAM_PIN_D7,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,

        .xclk_freq_hz = CONFIG_CAMNODE_XCLK_MHZ * 1000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        /* The driver sizes its JPEG frame buffers from THIS value and never resizes
         * them, so init at the largest size we will ever use (changing to a bigger
         * size later overflows the buffers: "cam_hal: FB-OVF"). We drop to QVGA
         * right after init. */
        .frame_size   = k_sizes[N_SIZES - 1],
        .jpeg_quality = CONFIG_CAMNODE_JPEG_QUALITY,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = 0,
    };

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    s.init_err = esp_camera_init(&cfg);
    if (s.init_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", s.init_err, esp_err_to_name(s.init_err));
        return s.init_err;
    }

    sensor_t *sen = esp_camera_sensor_get();
    s.pid = sen->id.PID;
    ESP_LOGI(TAG, "sensor PID=0x%04x (OV3660 = 0x%04x)", s.pid, OV3660_PID);

    /* Kconfig bools are only defined when enabled, so test with #ifdef */
#ifdef CONFIG_CAMNODE_VFLIP
    sen->set_vflip(sen, 1);
#else
    sen->set_vflip(sen, 0);
#endif
#ifdef CONFIG_CAMNODE_HMIRROR
    sen->set_hmirror(sen, 1);
#else
    sen->set_hmirror(sen, 0);
#endif
    if (s.pid == OV3660_PID) {
        sen->set_brightness(sen, 1);
        sen->set_saturation(sen, -2);
    }

    /* Buffers are now sized for the largest frame; start small (BLE friendly). */
    if (sen->set_framesize(sen, k_sizes[0]) != 0) {
        ESP_LOGW(TAG, "could not switch to initial QVGA size");
    }
    s.size_idx = 0;
    s.ok = true;
    vTaskDelay(pdMS_TO_TICKS(150));
    flush_stale_frames();
    return ESP_OK;
}

camera_fb_t *camsvc_capture(void)
{
    if (!s.ok) {
        return NULL;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        s.frames++;
        s.last_len = fb->len;
        s.last_w = fb->width;
        s.last_h = fb->height;
    }
    return fb;
}

void camsvc_release(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

esp_err_t camsvc_set_size_index(uint8_t idx)
{
    if (!s.ok || idx >= N_SIZES) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_t *sen = esp_camera_sensor_get();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = sen->set_framesize(sen, k_sizes[idx]);
    xSemaphoreGive(s_lock);
    if (rc != 0) {
        ESP_LOGE(TAG, "set_framesize(%u) failed rc=%d", idx, rc);
        return ESP_FAIL;
    }
    s.size_idx = idx;
    vTaskDelay(pdMS_TO_TICKS(150));      /* let the sensor settle on the new mode */
    flush_stale_frames();
    ESP_LOGI(TAG, "frame size index -> %u (sensor reports framesize %d)", idx,
             (int)sen->status.framesize);
    return ESP_OK;
}

esp_err_t camsvc_set_quality(uint8_t q)
{
    if (!s.ok || q > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Very high quality can exceed the driver's JPEG buffer (1/5 of raw size). */
    if (q < 8) {
        ESP_LOGW(TAG, "quality %u clamped to 8", q);
        q = 8;
    }
    sensor_t *sen = esp_camera_sensor_get();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = sen->set_quality(sen, q);
    xSemaphoreGive(s_lock);
    if (rc != 0) {
        return ESP_FAIL;
    }
    s.quality = q;
    flush_stale_frames();
    ESP_LOGI(TAG, "JPEG quality -> %u", q);
    return ESP_OK;
}

esp_err_t camsvc_read_light(int *light_pct, uint32_t *exp_raw, uint32_t *gain_raw)
{
    if (!s.ok) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *sen = esp_camera_sensor_get();

    /* OV3660 auto-exposure read-back: exposure 0x3500..0x3502, gain 0x350A/0x350B. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int e0 = sen->get_reg(sen, 0x3500, 0x0f);
    int e1 = sen->get_reg(sen, 0x3501, 0xff);
    int e2 = sen->get_reg(sen, 0x3502, 0xff);
    int g0 = sen->get_reg(sen, 0x350a, 0x03);
    int g1 = sen->get_reg(sen, 0x350b, 0xff);
    xSemaphoreGive(s_lock);
    if (e0 < 0 || e1 < 0 || e2 < 0 || g0 < 0 || g1 < 0) {
        return ESP_FAIL;
    }

    uint32_t e = ((uint32_t)e0 << 16) | ((uint32_t)e1 << 8) | (uint32_t)e2;
    uint32_t g = ((uint32_t)g0 << 8) | (uint32_t)g1;

    /* How hard the sensor works = exposure x gain. Log scale, mapped so that
     * 2^10 -> 0 % dark and 2^28 -> 100 % dark. Uncalibrated: tune with the slider. */
    float prod = (float)(e ? e : 1) * (float)(g + 16);
    float dark = (log2f(prod) - 10.0f) / 18.0f * 100.0f;
    if (dark < 0) dark = 0;
    if (dark > 100) dark = 100;

    *light_pct = 100 - (int)dark;
    *exp_raw = e;
    *gain_raw = g;
    return ESP_OK;
}

esp_err_t camsvc_self_test(void)
{
    if (!s.ok) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    const int n = 3;
    int64_t t0 = esp_timer_get_time();

    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = camsvc_capture();
        if (!fb) {
            ESP_LOGE(TAG, "self-test: frame %d capture failed", i);
            result = ESP_FAIL;
            continue;
        }
        bool soi = fb->len > 4 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8;
        bool eoi = fb->len > 4 && fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
        ESP_LOGI(TAG, "self-test: frame %d  %ux%u  %u bytes  SOI=%d EOI=%d",
                 i, (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len, soi, eoi);
        if (!soi || !eoi) {
            result = ESP_ERR_INVALID_RESPONSE;
        }
        camsvc_release(fb);
    }

    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "self-test: %d frames in %lld ms -> %s", n, (long long)dt_ms,
             result == ESP_OK ? "PASS" : "FAIL");
    return result;
}

void camsvc_status_string(char *buf, size_t len)
{
    if (!s.ok) {
        snprintf(buf, len, "camera NOT initialised (err=0x%x %s)", s.init_err,
                 esp_err_to_name(s.init_err));
        return;
    }
    snprintf(buf, len,
             "sensor=%s pid=0x%04x fmt=JPEG size_idx=%u q=%u last=%ux%u/%luB frames=%lu psram_free=%u",
             s.pid == OV3660_PID ? "OV3660" : "unknown", s.pid, s.size_idx, s.quality,
             s.last_w, s.last_h, (unsigned long)s.last_len, (unsigned long)s.frames,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
