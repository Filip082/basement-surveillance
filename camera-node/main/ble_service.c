#include "ble_service.h"

#include <string.h>
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "board_dfr1154.h"
#include "camera_service.h"
#include "board_io.h"
#include "daynight.h"

static const char *TAG = "ble";

/* ------------------------------------------------------------------ UUIDs */

/* dfa100NN-5b3c-4f6d-9e21-0a4c7b8d1e00 (NimBLE wants little-endian bytes) */
#define CAM_UUID128(n) \
    BLE_UUID128_INIT(0x00, 0x1e, 0x8d, 0x7b, 0x4c, 0x0a, 0x21, 0x9e, \
                     0x6d, 0x4f, 0x3c, 0x5b, (n), 0x00, 0xa1, 0xdf)

static const ble_uuid128_t UUID_SVC    = CAM_UUID128(0x01);
static const ble_uuid128_t UUID_CTRL   = CAM_UUID128(0x02);
static const ble_uuid128_t UUID_IMAGE  = CAM_UUID128(0x03);
static const ble_uuid128_t UUID_STATUS = CAM_UUID128(0x04);

/* ---------------------------------------------------------------- protocol */

/* Control commands (write to Control characteristic: [id][arg]) */
enum {
    CMD_CAPTURE   = 0x01,   /* no arg: capture one JPEG and stream it on Image */
    CMD_SIZE      = 0x02,   /* arg: 0=QVGA 1=VGA 2=SVGA 3=XGA 4=SXGA 5=UXGA */
    CMD_QUALITY   = 0x03,   /* arg: 4 (best) .. 63 (smallest) */
    CMD_LED       = 0x10,   /* arg: 0/1  on-board LED (GPIO3) */
    CMD_IR        = 0x11,   /* arg: 0/1  IR illumination (GPIO47); also turns auto mode off */
    CMD_IR_AUTO   = 0x12,   /* arg: 0/1  automatic day/night IR control */
    CMD_IR_THRESH = 0x13,   /* arg: 0..100  light level below which it is "night" */
};

/* Image notification packet types */
enum {
    PKT_START = 0x01,   /* [01][len u32][w u16][h u16]  */
    PKT_DATA  = 0x02,   /* [02][seq u16][jpeg bytes...] */
    PKT_END   = 0x03,   /* [03][crc32 u32] (zlib CRC32 of the whole JPEG) */
};

typedef struct {
    uint8_t id;
    uint8_t arg;
} cmd_t;

/* ------------------------------------------------------------------- state */

static QueueHandle_t s_cmd_q;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_img_sub;
static uint16_t s_img_handle;
static uint8_t s_own_addr_type;

static int gap_event(struct ble_gap_event *event, void *arg);

/* ------------------------------------------------------------- GATT access */

static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[4] = {0};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1 || len > sizeof(buf)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &copied) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    cmd_t c = { .id = buf[0], .arg = len > 1 ? buf[1] : 0 };
    if (xQueueSend(s_cmd_q, &c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, dropped 0x%02x", c.id);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int image_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Notify-only characteristic: no direct reads or writes. */
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int status_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char line[360];
    camsvc_status_string(line, sizeof(line));
    size_t n = strlen(line);
    daynight_status_string(line + n, sizeof(line) - n);
    return os_mbuf_append(ctxt->om, line, strlen(line)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &UUID_CTRL.u,
                .access_cb = ctrl_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &UUID_IMAGE.u,
                .access_cb = image_access,
                .val_handle = &s_img_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &UUID_STATUS.u,
                .access_cb = status_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

/* --------------------------------------------------------- image streaming */

/* Send one notification, retrying while the host/controller buffers are full. */
static bool notify_retry(const uint8_t *data, size_t len)
{
    for (int tries = 0; tries < 400; tries++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_img_sub) {
            return false;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn, s_img_handle, om);   /* consumes om */
            if (rc == 0) {
                return true;
            }
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                ESP_LOGW(TAG, "notify failed rc=%d", rc);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGW(TAG, "notify timed out");
    return false;
}

static void send_frame(const camera_fb_t *fb)
{
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < 23) {
        mtu = 23;
    }
    const size_t chunk = mtu - 3 /* ATT header */ - 3 /* our header */;
    uint8_t pkt[3 + 512];
    if (chunk > sizeof(pkt) - 3) {
        ESP_LOGE(TAG, "MTU %u too large for buffer", mtu);
        return;
    }

    uint8_t start[9] = { PKT_START };
    memcpy(&start[1], &(uint32_t){ (uint32_t)fb->len }, 4);          /* little-endian on ESP32 */
    memcpy(&start[5], &(uint16_t){ (uint16_t)fb->width }, 2);
    memcpy(&start[7], &(uint16_t){ (uint16_t)fb->height }, 2);
    if (!notify_retry(start, sizeof(start))) {
        return;
    }

    int64_t t0 = esp_timer_get_time();
    uint16_t seq = 0;
    for (size_t off = 0; off < fb->len; off += chunk, seq++) {
        size_t n = fb->len - off < chunk ? fb->len - off : chunk;
        pkt[0] = PKT_DATA;
        memcpy(&pkt[1], &seq, 2);
        memcpy(&pkt[3], fb->buf + off, n);
        if (!notify_retry(pkt, 3 + n)) {
            ESP_LOGW(TAG, "transfer aborted at %u/%u bytes", (unsigned)off, (unsigned)fb->len);
            return;
        }
    }

    uint32_t crc = esp_rom_crc32_le(0, fb->buf, fb->len);
    uint8_t end[5] = { PKT_END };
    memcpy(&end[1], &crc, 4);
    notify_retry(end, sizeof(end));

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "sent %u bytes in %lld ms (%u B/packet, MTU %u, crc=0x%08x)",
             (unsigned)fb->len, (long long)ms, (unsigned)chunk, mtu, (unsigned)crc);
}

static void do_capture(void)
{
    camera_fb_t *fb = camsvc_capture();
    if (!fb) {
        ESP_LOGE(TAG, "capture failed");
        return;
    }
    ESP_LOGI(TAG, "captured %ux%u, %u bytes", (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_img_sub) {
        send_frame(fb);
    } else {
        ESP_LOGW(TAG, "not streaming: no client with Image notifications enabled");
    }
    camsvc_release(fb);
}

/* Commands run here (not in the NimBLE host task) so slow camera work and
 * long notification bursts never block the BLE stack. */
static void worker_task(void *arg)
{
    cmd_t c;
    for (;;) {
        xQueueReceive(s_cmd_q, &c, portMAX_DELAY);
        ESP_LOGI(TAG, "cmd 0x%02x arg %u", c.id, c.arg);
        switch (c.id) {
        case CMD_CAPTURE:
            do_capture();
            break;
        case CMD_SIZE:
            if (camsvc_set_size_index(c.arg) != ESP_OK) {
                ESP_LOGW(TAG, "bad size index %u", c.arg);
            }
            break;
        case CMD_QUALITY:
            if (camsvc_set_quality(c.arg) != ESP_OK) {
                ESP_LOGW(TAG, "bad quality %u", c.arg);
            }
            break;
        case CMD_LED:
            board_led_set(c.arg != 0);
            break;
        case CMD_IR:
            daynight_set_auto(false);           /* manual override */
            board_ir_set(c.arg != 0);
            break;
        case CMD_IR_AUTO:
            daynight_set_auto(c.arg != 0);
            break;
        case CMD_IR_THRESH:
            daynight_set_threshold(c.arg);
            break;
        default:
            ESP_LOGW(TAG, "unknown command 0x%02x", c.id);
        }
    }
}

/* --------------------------------------------------------------- GAP / adv */

static void start_advertising(void)
{
    const char *name = ble_svc_gap_device_name();

    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (const uint8_t *)name;
    adv.name_len = strlen(name);
    adv.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    /* 128-bit UUID does not fit next to the name: put it in the scan response. */
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", name);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect: status=%d handle=%d", event->connect.status, event->connect.conn_handle);
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            /* MTU exchange is started by the central (Chrome / phone); the GATT client API
             * is not built because the central role is disabled in sdkconfig.defaults. */
            ble_gap_set_data_len(s_conn, 251, 2120);             /* longer link-layer packets */
        } else {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect: reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_img_sub = false;
        start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_img_handle) {
            s_img_sub = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Image notifications %s", s_img_sub ? "enabled" : "disabled");
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %u", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    start_advertising();
}

static void host_task(void *param)
{
    nimble_port_run();                  /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ public */

esp_err_t ble_service_start(void)
{
    ESP_LOGI(TAG, "start: creating worker");
    s_cmd_q = xQueueCreate(8, sizeof(cmd_t));
    if (!s_cmd_q) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(worker_task, "cam_ble_worker", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "start: nimble_port_init");
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "start: nimble_port_init done, registering GATT");
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed rc=%d", rc);
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(CONFIG_CAMNODE_BLE_DEVICE_NAME);

    ESP_LOGI(TAG, "start: launching host task");
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
