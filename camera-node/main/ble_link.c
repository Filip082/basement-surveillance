#include "ble_link.h"

#include <string.h>
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

/* Not declared in any public header (IDF examples declare it the same way). */
extern void ble_store_config_init(void);

#include "node_log.h"

#define T "ble"

/* ------------------------------------------------------------------ UUIDs */

/* dfa100NN-5b3c-4f6d-9e21-0a4c7b8d1e00 (NimBLE wants little-endian bytes) */
#define CAM_UUID128(n) \
    BLE_UUID128_INIT(0x00, 0x1e, 0x8d, 0x7b, 0x4c, 0x0a, 0x21, 0x9e, \
                     0x6d, 0x4f, 0x3c, 0x5b, (n), 0x00, 0xa1, 0xdf)

static const ble_uuid128_t UUID_SVC   = CAM_UUID128(0x01);
static const ble_uuid128_t UUID_CTRL  = CAM_UUID128(0x02);
static const ble_uuid128_t UUID_IMAGE = CAM_UUID128(0x03);

/* Image notification packet types (little-endian fields) */
enum {
    PKT_START = 0x01,   /* [01][len u32][w u16][h u16]  */
    PKT_DATA  = 0x02,   /* [02][seq u16][jpeg bytes...] */
    PKT_END   = 0x03,   /* [03][crc32 u32]  zlib CRC32 of the whole JPEG */
};

/* ------------------------------------------------------------------ state */

enum {
    EV_SYNCED       = 1u << 0,
    EV_CONNECTED    = 1u << 1,
    EV_ENCRYPTED    = 1u << 2,
    EV_SUBSCRIBED   = 1u << 3,
    EV_DISCONNECTED = 1u << 4,
    EV_ACK          = 1u << 5,
    EV_NAK          = 1u << 6,
};

#ifdef CONFIG_NODE_BLE_REQUIRE_ENCRYPTION
#define NEED_READY (EV_SUBSCRIBED | EV_ENCRYPTED)
#else
#define NEED_READY (EV_SUBSCRIBED)
#endif

static EventGroupHandle_t s_ev;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_img_handle;
static uint8_t s_own_addr_type;
static volatile bool s_running;
static volatile bool s_adv_wanted;

static int gap_event(struct ble_gap_event *event, void *arg);

/* ------------------------------------------------------------- GATT access */

/* Control characteristic: the computer confirms or rejects the frame. */
static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t cmd = 0;
    uint16_t copied = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) < 1 ||
        ble_hs_mbuf_to_flat(ctxt->om, &cmd, 1, &copied) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (cmd == 0x01) {
        xEventGroupSetBits(s_ev, EV_ACK);
    } else if (cmd == 0x02) {
        xEventGroupSetBits(s_ev, EV_NAK);
    }
    return 0;
}

/* Image characteristic is notify-only. */
static int image_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
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
            { 0 },
        },
    },
    { 0 },
};

/* --------------------------------------------------------------- advertising */

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
        NLOGE(T, "adv_set_fields rc=%d", rc);
        return;
    }

    /* The 128-bit UUID does not fit next to the name: put it in the scan response. */
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        NLOGE(T, "adv_rsp_set_fields rc=%d", rc);
        return;
    }

    /* Short interval = the computer finds us (and connects) as fast as possible. */
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(20);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(30);
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        NLOGE(T, "adv_start rc=%d", rc);
        return;
    }
    NLOGI(T, "advertising as \"%s\"", name);
}

/* ---------------------------------------------------------------- GAP events */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        NLOGI(T, "connect: status=%d handle=%d", event->connect.status, event->connect.conn_handle);
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            xEventGroupClearBits(s_ev, EV_DISCONNECTED | EV_ACK | EV_NAK);
            xEventGroupSetBits(s_ev, EV_CONNECTED);

            /* Best effort speed-ups: long packets, short connection interval (7.5-15 ms). */
            ble_gap_set_data_len(s_conn, 251, 2120);
            struct ble_gap_upd_params up = {
                .itvl_min = 6, .itvl_max = 12, .latency = 0,
                .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0,
            };
            ble_gap_update_params(s_conn, &up);

            /* Ask for pairing / encryption (bonded computers simply re-encrypt). */
#ifdef CONFIG_NODE_BLE_REQUIRE_ENCRYPTION
            int rc = ble_gap_security_initiate(s_conn);
            NLOGI(T, "security request sent rc=%d", rc);
#endif
        } else if (s_adv_wanted) {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        NLOGI(T, "disconnect: reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        xEventGroupClearBits(s_ev, EV_CONNECTED | EV_ENCRYPTED | EV_SUBSCRIBED);
        xEventGroupSetBits(s_ev, EV_DISCONNECTED);
        if (s_adv_wanted) {
            start_advertising();            /* the computer may reconnect within the window */
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_adv_wanted) {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_img_handle) {
            NLOGI(T, "Image notifications %s", event->subscribe.cur_notify ? "enabled" : "disabled");
            if (event->subscribe.cur_notify) {
                xEventGroupSetBits(s_ev, EV_SUBSCRIBED);
            } else {
                xEventGroupClearBits(s_ev, EV_SUBSCRIBED);
            }
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            int bonded = 0;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                bonded = desc.sec_state.bonded;
            }
            NLOGI(T, "link encrypted (bonded=%d)", bonded);
            xEventGroupSetBits(s_ev, EV_ENCRYPTED);
        } else {
            NLOGW(T, "encryption failed, status=%d", event->enc_change.status);
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The computer lost its key but we still have the old one: replace it. */
        NLOGW(T, "repeat pairing - deleting the old bond and retrying");
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

#ifdef CONFIG_NODE_BLE_PAIRING_PASSKEY
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io pk = {0};
            pk.action = event->passkey.params.action;
            pk.passkey = CONFIG_NODE_BLE_PASSKEY;
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &pk);
            NLOGI(T, "passkey injected rc=%d", rc);
        }
        break;
#endif

    case BLE_GAP_EVENT_MTU:
        NLOGI(T, "MTU updated: %u", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

/* ----------------------------------------------------------- stack lifecycle */

static void on_reset(int reason)
{
    NLOGE(T, "host reset, reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        NLOGE(T, "address setup failed rc=%d", rc);
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    NLOGI(T, "stack ready, address %02x:%02x:%02x:%02x:%02x:%02x",
          addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    xEventGroupSetBits(s_ev, EV_SYNCED);
    if (s_adv_wanted) {
        start_advertising();
    }
}

static void host_task(void *param)
{
    nimble_port_run();                  /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void ble_link_erase_bonds(void)
{
    /* NimBLE keeps its pairing keys in this NVS namespace. */
    nvs_handle_t h;
    if (nvs_open("nimble_bond", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        NLOGI(T, "all stored pairings erased");
    }
}

esp_err_t ble_link_start(bool advertise)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_ev) {
        s_ev = xEventGroupCreate();
    }
    xEventGroupClearBits(s_ev, 0xFFFFFF);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_adv_wanted = advertise;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        NLOGE(T, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Pairing: LE Secure Connections with bonding (keys saved in NVS). */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
#ifdef CONFIG_NODE_BLE_PAIRING_PASSKEY
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;      /* we "display" the fixed passkey */
    ble_hs_cfg.sm_mitm = 1;
#else
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   /* Just Works */
    ble_hs_cfg.sm_mitm = 0;
#endif

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc != 0) {
        NLOGE(T, "GATT registration failed rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(CONFIG_NODE_BLE_DEVICE_NAME);
    ble_store_config_init();

    s_running = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t ble_link_wait_synced(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_SYNCED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (b & EV_SYNCED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

int ble_link_bond_count(void)
{
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
    return n;
}

esp_err_t ble_link_wait_ready(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_ev, NEED_READY, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if ((b & NEED_READY) == NEED_READY) {
        return ESP_OK;
    }
    NLOGW(T, "no usable computer within %u ms (connected=%d encrypted=%d subscribed=%d)",
          (unsigned)timeout_ms, !!(b & EV_CONNECTED), !!(b & EV_ENCRYPTED), !!(b & EV_SUBSCRIBED));
    return ESP_ERR_TIMEOUT;
}

void ble_link_stop(void)
{
    if (!s_running) {
        return;
    }
    s_adv_wanted = false;

    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        xEventGroupClearBits(s_ev, EV_DISCONNECTED);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        xEventGroupWaitBits(s_ev, EV_DISCONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(500));
    }
    ble_gap_adv_stop();                 /* harmless if not advertising */

    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
        NLOGI(T, "stack shut down");
    } else {
        NLOGE(T, "nimble_port_stop failed");
    }
    s_running = false;
}

/* ------------------------------------------------------------ image streaming */

/* Send one notification, retrying while the host/controller buffers are full. */
static bool notify_retry(const uint8_t *data, size_t len)
{
    for (int tries = 0; tries < 2000; tries++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            return false;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn, s_img_handle, om);   /* consumes om */
            if (rc == 0) {
                return true;
            }
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                NLOGW(T, "notify failed rc=%d", rc);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    NLOGW(T, "notify timed out");
    return false;
}

esp_err_t ble_link_send_image(const uint8_t *jpeg, size_t len, uint16_t width, uint16_t height)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_ev, EV_ACK | EV_NAK);

    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < 23) {
        mtu = 23;
    }
    const size_t chunk = mtu - 3 /* ATT header */ - 3 /* our header */;
    uint8_t pkt[3 + 512];
    if (chunk > sizeof(pkt) - 3) {
        NLOGE(T, "MTU %u too large for buffer", mtu);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t start[9] = { PKT_START };
    uint32_t len32 = (uint32_t)len;
    memcpy(&start[1], &len32, 4);                         /* ESP32 is little-endian */
    memcpy(&start[5], &width, 2);
    memcpy(&start[7], &height, 2);
    if (!notify_retry(start, sizeof(start))) {
        return ESP_FAIL;
    }

    int64_t t0 = esp_timer_get_time();
    uint16_t seq = 0;
    for (size_t off = 0; off < len; off += chunk, seq++) {
        size_t n = len - off < chunk ? len - off : chunk;
        pkt[0] = PKT_DATA;
        memcpy(&pkt[1], &seq, 2);
        memcpy(&pkt[3], jpeg + off, n);
        if (!notify_retry(pkt, 3 + n)) {
            NLOGW(T, "transfer aborted at %u/%u bytes", (unsigned)off, (unsigned)len);
            return ESP_FAIL;
        }
    }

    uint32_t crc = esp_rom_crc32_le(0, jpeg, len);        /* zlib-compatible CRC32 */
    uint8_t end[5] = { PKT_END };
    memcpy(&end[1], &crc, 4);
    if (!notify_retry(end, sizeof(end))) {
        return ESP_FAIL;
    }

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    NLOGI(T, "sent %u bytes in %lld ms (%u B/packet, MTU %u, crc=0x%08x)",
          (unsigned)len, (long long)ms, (unsigned)chunk, mtu, (unsigned)crc);
    return ESP_OK;
}

ble_ack_t ble_link_wait_ack(uint32_t timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_ACK | EV_NAK, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    if (b & EV_ACK) {
        return BLE_ACK_OK;
    }
    if (b & EV_NAK) {
        return BLE_ACK_NAK;
    }
    return BLE_ACK_TIMEOUT;
}
