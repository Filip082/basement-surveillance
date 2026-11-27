/*
 * Person + face detection with esp-dl (espressif/pedestrian_detect and
 * espressif/human_face_detect). The libraries are C++, so this small file wraps them
 * behind the C interface of vision.h; the rest of the project stays C.
 */
#include "vision.h"

#include <stdlib.h>
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "human_face_detect.hpp"
#include "img_converters.h"   /* fmt2jpg() from esp32-camera */
#include "sdkconfig.h"

#ifdef CONFIG_NODE_PERSON_DETECT
#include "pedestrian_detect.hpp"
static PedestrianDetect *s_person;
#endif

static const char *TAG = "vision";
static HumanFaceDetect *s_face;

#define MAX_BOXES 8
struct box_t { int x1, y1, x2, y2; };

esp_err_t vision_init(void)
{
    int64_t t0 = esp_timer_get_time();
    if (!s_face) {
        s_face = new HumanFaceDetect();
        if (!s_face) {
            return ESP_ERR_NO_MEM;
        }
    }
#ifdef CONFIG_NODE_PERSON_DETECT
    if (!s_person) {
        s_person = new PedestrianDetect();
        if (!s_person) {
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    ESP_LOGI(TAG, "models loaded in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

/* Draw an outline rectangle (thickness t) into an RGB888 buffer (R,G,B byte order). */
static void draw_rect(uint8_t *rgb, int w, int h, box_t b, uint32_t color, int t)
{
    const uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, bl = color & 0xFF;
    int x1 = b.x1 < 0 ? 0 : b.x1, y1 = b.y1 < 0 ? 0 : b.y1;
    int x2 = b.x2 > w - 1 ? w - 1 : b.x2, y2 = b.y2 > h - 1 ? h - 1 : b.y2;
    if (x2 <= x1 || y2 <= y1) {
        return;
    }
    for (int y = y1; y <= y2; y++) {
        const bool edge_row = (y - y1 < t) || (y2 - y < t);
        uint8_t *row = rgb + (size_t)y * w * 3;
        for (int x = x1; x <= x2; x++) {
            if (edge_row || (x - x1 < t) || (x2 - x < t)) {
                row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = bl;
            }
        }
    }
}

/* Keep results above the score threshold (percent); returns how many, fills boxes/best. */
static int collect(std::list<dl::detect::result_t> &results, int min_pct, box_t *boxes, float *best)
{
    int n = 0;
    for (const auto &r : results) {
        if (r.score * 100.0f < (float)min_pct) {
            continue;
        }
        if (n < MAX_BOXES) {
            boxes[n] = { r.box[0], r.box[1], r.box[2], r.box[3] };
        }
        n++;
        if (r.score > *best) {
            *best = r.score;
        }
    }
    return n;
}

esp_err_t vision_analyze(const uint8_t *jpeg, size_t jpeg_len, vision_result_t *res)
{
    *res = {};
    if (!s_face) {
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

    box_t human_boxes[MAX_BOXES], face_boxes[MAX_BOXES];
    bool person_stage = false;

#ifdef CONFIG_NODE_PERSON_DETECT
    person_stage = true;
    if (s_person) {
        res->humans = collect(s_person->run(img), CONFIG_NODE_PERSON_MIN_SCORE_PCT, human_boxes,
                              &res->human_score);
    }
#endif
    int64_t t2 = esp_timer_get_time();

    bool run_face = !person_stage || res->humans > 0;
#ifdef CONFIG_NODE_FACE_WITHOUT_PERSON
    run_face = true;
#endif
    if (run_face) {
        res->faces = collect(s_face->run(img), CONFIG_NODE_FACE_MIN_SCORE_PCT, face_boxes,
                             &res->face_score);
        if (res->faces > 0 && res->humans == 0) {
            res->humans = res->faces;   /* a face is a person even if the body detector missed it */
        }
    }
    int64_t t3 = esp_timer_get_time();
    ESP_LOGI(TAG, "%d person(s) %.2f, %d face(s) %.2f (decode %lld, person %lld, face %lld ms)",
             res->humans, (double)res->human_score, res->faces, (double)res->face_score,
             (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000),
             (long long)((t3 - t2) / 1000));

    /* frames: faces win, otherwise the people */
    uint8_t *rgb = (uint8_t *)img.data;
    const int w = img.width, h = img.height;
    bool drawn = false;
    if (res->faces > 0) {
        for (int i = 0; i < res->faces && i < MAX_BOXES; i++) {
            draw_rect(rgb, w, h, face_boxes[i], CONFIG_NODE_FACE_BOX_COLOR, CONFIG_NODE_FACE_BOX_THICKNESS);
        }
        drawn = true;
    }
#if defined(CONFIG_NODE_PERSON_DETECT) && defined(CONFIG_NODE_PERSON_DRAW_BOX)
    else if (res->humans > 0) {
        for (int i = 0; i < res->humans && i < MAX_BOXES; i++) {
            draw_rect(rgb, w, h, human_boxes[i], CONFIG_NODE_PERSON_BOX_COLOR, CONFIG_NODE_FACE_BOX_THICKNESS);
        }
        drawn = true;
    }
#endif

    if (drawn) {
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
            ESP_LOGI(TAG, "frames drawn, re-encoded in %lld ms (%u bytes)",
                     (long long)((esp_timer_get_time() - t3) / 1000), (unsigned)out_len);
        } else {
            ESP_LOGW(TAG, "re-encoding failed - the original picture will be sent");
        }
    }
    heap_caps_free(img.data);
    return ESP_OK;
}
