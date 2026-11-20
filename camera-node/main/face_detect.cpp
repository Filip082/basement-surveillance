/*
 * Face detection with espressif/human_face_detect (esp-dl). The library is C++, so this
 * small file wraps it behind the C interface of face_detect.h; the rest of the project
 * stays C.
 */
#include "face_detect.h"

#include <stdlib.h>
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "human_face_detect.hpp"
#include "img_converters.h"   /* fmt2jpg() from esp32-camera */
#include "sdkconfig.h"

static const char *TAG = "face";
static HumanFaceDetect *s_detector;

esp_err_t face_detect_init(void)
{
    if (s_detector) {
        return ESP_OK;
    }
    int64_t t0 = esp_timer_get_time();
    s_detector = new HumanFaceDetect();
    if (!s_detector) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "face detector loaded in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

/* Draw an outline rectangle (thickness t) into an RGB888 buffer (R,G,B byte order). */
static void draw_rect(uint8_t *rgb, int w, int h, int x1, int y1, int x2, int y2, uint32_t color, int t)
{
    const uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > w - 1) x2 = w - 1;
    if (y2 > h - 1) y2 = h - 1;
    if (x2 <= x1 || y2 <= y1) {
        return;
    }
    for (int y = y1; y <= y2; y++) {
        const bool edge_row = (y - y1 < t) || (y2 - y < t);
        uint8_t *row = rgb + (size_t)y * w * 3;
        if (edge_row) {
            for (int x = x1; x <= x2; x++) {
                row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
            }
        } else {
            for (int x = x1; x <= x2; x++) {
                if ((x - x1 < t) || (x2 - x < t)) {
                    row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
                }
            }
        }
    }
}

esp_err_t face_detect_run(const uint8_t *jpeg, size_t jpeg_len, bool draw_frames, face_result_t *res)
{
    res->faces = 0;
    res->best_score = 0;
    res->jpeg = NULL;
    res->jpeg_len = 0;
    if (!s_detector) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t t0 = esp_timer_get_time();
    dl::image::jpeg_img_t jpeg_img = {};
    jpeg_img.data = (void *)jpeg;
    jpeg_img.data_len = jpeg_len;
    auto img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (!img.data) {
        ESP_LOGE(TAG, "JPEG decode failed");
        return ESP_FAIL;
    }
    int64_t t1 = esp_timer_get_time();

    auto &results = s_detector->run(img);
    int64_t t2 = esp_timer_get_time();

    for (const auto &r : results) {
        res->faces++;
        if (r.score > res->best_score) {
            res->best_score = r.score;
        }
    }
    ESP_LOGI(TAG, "%d face(s), best score %.2f (decode %lld ms, detect %lld ms)", res->faces,
             (double)res->best_score, (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000));

    esp_err_t ret = ESP_OK;
    if (res->faces > 0 && draw_frames) {
        uint8_t *rgb = (uint8_t *)img.data;
        const int w = img.width, h = img.height;
        for (const auto &r : results) {
            draw_rect(rgb, w, h, r.box[0], r.box[1], r.box[2], r.box[3],
                      CONFIG_NODE_FACE_BOX_COLOR, CONFIG_NODE_FACE_BOX_THICKNESS);
        }
#ifdef CONFIG_NODE_FACE_ENCODER_BGR
        for (size_t i = 0; i < (size_t)w * h * 3; i += 3) {   /* RGB -> BGR for the encoder */
            uint8_t tmp = rgb[i];
            rgb[i] = rgb[i + 2];
            rgb[i + 2] = tmp;
        }
#endif
        uint8_t *out = NULL;
        size_t out_len = 0;
        if (fmt2jpg(rgb, (size_t)w * h * 3, w, h, PIXFORMAT_RGB888, CONFIG_NODE_FACE_JPEG_QUALITY,
                    &out, &out_len) && out) {
            res->jpeg = out;
            res->jpeg_len = out_len;
            ESP_LOGI(TAG, "frame drawn, re-encoded in %lld ms (%u bytes)",
                     (long long)((esp_timer_get_time() - t2) / 1000), (unsigned)out_len);
        } else {
            ESP_LOGW(TAG, "re-encoding failed - the original picture will be sent");
            ret = ESP_OK;
        }
    }
    heap_caps_free(img.data);
    return ret;
}
