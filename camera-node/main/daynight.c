#include "daynight.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "board_io.h"
#include "als.h"
#include "camera_service.h"

static const char *TAG = "daynight";

#define SAMPLE_MS      1000
#define CONFIRM_COUNT  3
#define MIN_DWELL_MS   5000

#ifdef CONFIG_CAMNODE_AUTO_DAYNIGHT
static volatile bool s_auto = true;
#else
static volatile bool s_auto = false;
#endif
static volatile uint8_t s_thr = CONFIG_CAMNODE_DAYNIGHT_THRESHOLD;
static volatile int s_light = -1;
static volatile int s_eff = -1;
static volatile uint32_t s_exp, s_gain;
static volatile bool s_src_als;
static volatile float s_lux = -1;

void daynight_set_auto(bool enable)
{
    if (s_auto != enable) {
        ESP_LOGI(TAG, "auto day/night %s", enable ? "ON" : "OFF");
    }
    s_auto = enable;
}

void daynight_set_threshold(uint8_t thr)
{
    if (thr > 100) {
        thr = 100;
    }
    s_thr = thr;
    ESP_LOGI(TAG, "threshold -> %u", thr);
}

static void daynight_task(void *arg)
{
    int dark_cnt = 0, bright_cnt = 0;
    int64_t last_change_ms = -MIN_DWELL_MS;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));

        int light;
        uint32_t e = s_exp, g = s_gain;
        float lux = -1;
        bool use_als = false;

        if (als_available() && als_read_lux(&lux) == ESP_OK) {
            /* Real ambient light: barely sees the IR LEDs, so no compensation needed. */
            light = als_lux_to_level(lux);
            use_als = true;
        } else if (camsvc_read_light(&light, &e, &g) != ESP_OK) {
            continue;                       /* no light source available yet */
        }
        bool ir = board_ir_get();
        int comp = use_als ? 0 : CONFIG_CAMNODE_IR_COMP;
        int eff = ir ? light - comp : light;
        s_src_als = use_als;
        s_lux = lux;
        s_light = light;
        s_eff = eff;
        s_exp = e;
        s_gain = g;

        if (!s_auto) {
            dark_cnt = bright_cnt = 0;
            continue;
        }

        int thr = s_thr;
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool may_change = (now_ms - last_change_ms) >= MIN_DWELL_MS;

        if (!ir) {
            dark_cnt = (eff < thr) ? dark_cnt + 1 : 0;
            bright_cnt = 0;
            if (dark_cnt >= CONFIRM_COUNT && may_change) {
                board_ir_set(true);
                last_change_ms = now_ms;
                dark_cnt = 0;
                ESP_LOGI(TAG, "NIGHT: light %d < thr %d -> IR ON", eff, thr);
            }
        } else {
            bright_cnt = (eff > thr + CONFIG_CAMNODE_DAYNIGHT_HYSTERESIS) ? bright_cnt + 1 : 0;
            dark_cnt = 0;
            if (bright_cnt >= CONFIRM_COUNT && may_change) {
                board_ir_set(false);
                last_change_ms = now_ms;
                bright_cnt = 0;
                ESP_LOGI(TAG, "DAY: light %d > thr %d + %d -> IR OFF", eff, thr,
                         CONFIG_CAMNODE_DAYNIGHT_HYSTERESIS);
            }
        }
    }
}

void daynight_start(void)
{
    xTaskCreate(daynight_task, "daynight", 3072, NULL, 3, NULL);
}

void daynight_status_string(char *buf, size_t len)
{
    bool ir = board_ir_get();
    snprintf(buf, len, " auto=%d thr=%u ir=%d mode=%s src=%s lux=%.1f light=%d eff=%d exp=%lu gain=%lu",
             s_auto ? 1 : 0, (unsigned)s_thr, ir ? 1 : 0, ir ? "night" : "day",
             s_src_als ? "als" : "cam", (double)s_lux,
             s_light, s_eff, (unsigned long)s_exp, (unsigned long)s_gain);
}
