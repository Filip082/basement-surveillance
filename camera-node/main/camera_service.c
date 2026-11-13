#include "camera_service.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor.h"
#include "sdkconfig.h"

#include "board_dfr1154.h"
#include "node_log.h"

static const framesize_t k_sizes[NODE_NUM_FRAME_SIZES] = {
    FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA,
    FRAMESIZE_XGA,  FRAMESIZE_SXGA, FRAMESIZE_UXGA,
};
static const char *const k_names[NODE_NUM_FRAME_SIZES] = {
    "QVGA 320x240", "VGA 640x480", "SVGA 800x600",
    "XGA 1024x768", "SXGA 1280x1024", "UXGA 1600x1200",
};

/* OV3660 SYSTEM_CTRL0: bit 6 = software power-down (registers keep their values) */
#define REG_SYSTEM_CTRL0  0x3008
#define SW_POWER_DOWN     0x40

static struct {
    bool inited;
    node_config_t cfg;
} s;

const char *camsvc_size_name(uint8_t idx)
{
    return idx < NODE_NUM_FRAME_SIZES ? k_names[idx] : "?";
}

esp_err_t camsvc_init(const node_config_t *cfg)
{
    s.cfg = *cfg;
    uint8_t idx = cfg->frame_size_idx < NODE_NUM_FRAME_SIZES ? cfg->frame_size_idx
                                                             : NODE_DEFAULT_FRAME_SIZE_IDX;
    /* The driver sizes its JPEG buffers from the init size and never resizes them
     * (too small -> "cam_hal: FB-OVF"). Init one size larger for headroom, then
     * switch down to the wanted size below. */
    uint8_t init_idx = (idx + 1 < NODE_NUM_FRAME_SIZES) ? idx + 1 : idx;

    const camera_config_t cc = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk  = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d0 = CAM_PIN_D0, .pin_d1 = CAM_PIN_D1, .pin_d2 = CAM_PIN_D2, .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4, .pin_d5 = CAM_PIN_D5, .pin_d6 = CAM_PIN_D6, .pin_d7 = CAM_PIN_D7,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,

        .xclk_freq_hz = CONFIG_NODE_CAMERA_XCLK_MHZ * 1000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = k_sizes[init_idx],
        .jpeg_quality = cfg->jpeg_quality,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = 0,
    };

    esp_err_t err = esp_camera_init(&cc);
    if (err != ESP_OK) {
        NLOGE("cam", "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
        return err;
    }

    sensor_t *sen = esp_camera_sensor_get();
    NLOGI("cam", "sensor PID=0x%04x (OV3660 = 0x%04x)", sen->id.PID, OV3660_PID);

    /* Kconfig bools exist as macros only when enabled, so test with #ifdef */
#ifdef CONFIG_NODE_CAMERA_VFLIP
    sen->set_vflip(sen, 1);
#else
    sen->set_vflip(sen, 0);
#endif
#ifdef CONFIG_NODE_CAMERA_HMIRROR
    sen->set_hmirror(sen, 1);
#else
    sen->set_hmirror(sen, 0);
#endif
    if (sen->id.PID == OV3660_PID) {
        sen->set_brightness(sen, 1);
        sen->set_saturation(sen, -2);
    }
    sen->set_framesize(sen, k_sizes[idx]);
    sen->set_quality(sen, cfg->jpeg_quality);

    s.inited = true;
    NLOGI("cam", "initialised: %s, JPEG quality %u, XCLK %d MHz", k_names[idx],
          cfg->jpeg_quality, CONFIG_NODE_CAMERA_XCLK_MHZ);
    camsvc_standby();
    return ESP_OK;
}

esp_err_t camsvc_reinit(const node_config_t *cfg)
{
    if (s.inited) {
        esp_camera_deinit();
        s.inited = false;
    }
    return camsvc_init(cfg);
}

void camsvc_standby(void)
{
    if (!s.inited) {
        return;
    }
    sensor_t *sen = esp_camera_sensor_get();
    sen->set_reg(sen, REG_SYSTEM_CTRL0, SW_POWER_DOWN, SW_POWER_DOWN);
    NLOGI("cam", "sensor in software standby");
}

esp_err_t camsvc_wake(void)
{
    if (!s.inited) {
        NLOGW("cam", "wake: camera not initialised, doing full init");
        esp_err_t err = camsvc_init(&s.cfg);
        if (err != ESP_OK) {
            return err;
        }
    }
    sensor_t *sen = esp_camera_sensor_get();
    sen->set_reg(sen, REG_SYSTEM_CTRL0, SW_POWER_DOWN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));          /* sensor needs a moment to restart streaming */
    NLOGI("cam", "sensor woken");
    return ESP_OK;
}

static bool is_complete_jpeg(const camera_fb_t *fb)
{
    return fb->len > 4 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
           fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
}

camera_fb_t *camsvc_capture(int settle_frames)
{
    if (!s.inited && camsvc_wake() != ESP_OK) {
        return NULL;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        bool frames_flow = true;
        for (int i = 0; i < settle_frames; i++) {
            camera_fb_t *junk = esp_camera_fb_get();
            if (!junk) {
                frames_flow = false;
                break;
            }
            esp_camera_fb_return(junk);
        }
        if (frames_flow) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb && is_complete_jpeg(fb)) {
                NLOGI("cam", "captured %ux%u, %u bytes (after %d settle frames)",
                      (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len, settle_frames);
                return fb;
            }
            if (fb) {
                NLOGW("cam", "incomplete JPEG (%u bytes), discarding", (unsigned)fb->len);
                esp_camera_fb_return(fb);
            }
        }
        /* Slow path: frames stopped coming (e.g. the capture DMA did not survive sleep). */
        NLOGW("cam", "capture failed (attempt %d) - re-initialising the camera", attempt + 1);
        if (camsvc_reinit(&s.cfg) != ESP_OK || camsvc_wake() != ESP_OK) {
            return NULL;
        }
    }
    return NULL;
}

void camsvc_release(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}
