#include "node_config.h"

#include <string.h>
#include "nvs.h"
#include "node_log.h"

#define NS        "node"
#define KEY       "cfg"
#define MAGIC     0x4E4F4445u   /* "NODE" */
#define VERSION   1

/* Layout of the blob in flash. Change VERSION when you change this. */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  configured;        /* the flag: 1 = a complete setup was saved */
    node_config_t cfg;
} stored_t;

static node_config_t s_cfg = {
    .frame_size_idx  = NODE_DEFAULT_FRAME_SIZE_IDX,
    .jpeg_quality    = NODE_DEFAULT_JPEG_QUALITY,
    .light_threshold = NODE_DEFAULT_LIGHT_THR,
};
static bool s_from_flash;

static bool valid(const node_config_t *c)
{
    return c->frame_size_idx < NODE_NUM_FRAME_SIZES &&
           c->jpeg_quality >= 8 && c->jpeg_quality <= 63 &&
           c->light_threshold <= 100;
}

static void use_defaults(void)
{
    s_cfg.frame_size_idx = NODE_DEFAULT_FRAME_SIZE_IDX;
    s_cfg.jpeg_quality = NODE_DEFAULT_JPEG_QUALITY;
    s_cfg.light_threshold = NODE_DEFAULT_LIGHT_THR;
    s_from_flash = false;
}

void node_config_load(void)
{
    use_defaults();

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        stored_t st;
        size_t len = sizeof(st);
        esp_err_t err = nvs_get_blob(h, KEY, &st, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(st) && st.magic == MAGIC &&
            st.version == VERSION && st.configured && valid(&st.cfg)) {
            s_cfg = st.cfg;
            s_from_flash = true;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            NLOGW("cfg", "stored config unusable (err=0x%x) - using defaults", err);
        }
    }

#if NODE_STORE_DEFAULTS_TO_FLASH
    if (!s_from_flash) {
        node_config_save(&s_cfg);
    }
#endif
    NLOGI("cfg", "config from %s: size_idx=%u quality=%u light_thr=%u",
          s_from_flash ? "FLASH" : "hard-coded defaults",
          s_cfg.frame_size_idx, s_cfg.jpeg_quality, s_cfg.light_threshold);
}

const node_config_t *node_config_get(void)
{
    return &s_cfg;
}

bool node_config_is_from_flash(void)
{
    return s_from_flash;
}

esp_err_t node_config_save(const node_config_t *cfg)
{
    if (!valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }
    stored_t st = { .magic = MAGIC, .version = VERSION, .configured = 1, .cfg = *cfg };

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY, &st, sizeof(st));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_cfg = *cfg;
        s_from_flash = true;
        NLOGI("cfg", "config saved to flash, configured flag set");
    } else {
        NLOGE("cfg", "saving config failed: 0x%x", err);
    }
    return err;
}

esp_err_t node_config_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    use_defaults();
    NLOGI("cfg", "stored config erased, back to defaults (%s)", esp_err_to_name(err));
    return err;
}
