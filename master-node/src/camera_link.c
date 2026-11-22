/* SPDX-License-Identifier: Apache-2.0 */

#include "camera_link.h"

#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include "image_store.h"

LOG_MODULE_REGISTER(camlink, LOG_LEVEL_DBG);

/* ------------------------------------------------------------------------ */
/* Protocol of the camera firmware (camera-node/main/ble_link.c)             */
/* ------------------------------------------------------------------------ */


static const struct bt_uuid_128 uuid_svc =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xdfa10001, 0x5b3c, 0x4f6d, 0x9e21, 0x0a4c7b8d1e00));
static const struct bt_uuid_128 uuid_ctrl =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xdfa10002, 0x5b3c, 0x4f6d, 0x9e21, 0x0a4c7b8d1e00));
static const struct bt_uuid_128 uuid_img =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xdfa10003, 0x5b3c, 0x4f6d, 0x9e21, 0x0a4c7b8d1e00));

#define PKT_START 0x01   /* [01][len u32][w u16][h u16][flags u8][faces u8][humans u8]
			  * (little endian; the last three bytes are optional, older cameras
			  * do not send them) */
#define IMG_FLAG_FACE  0x01  /* flags bit0: a face was detected (and framed) */
#define IMG_FLAG_HUMAN 0x02  /* flags bit1: a person was detected */
#define PKT_DATA  0x02   /* [02][seq u16][bytes ...]                      */
#define PKT_END   0x03   /* [03][crc32 u32]                               */
#define PKT_BATTERY 0x04 /* [04][percent u8][battery mV u16][pin mV u16]  battery report */

#define CTRL_ACK 0x01    /* write to the Control characteristic: picture received */
#define CTRL_NAK 0x02    /* ... : resend please */
#define CTRL_READY 0x03  /* ... : subscribed, I am listening - camera may start sending */

/* After the link is encrypted the camera is the one that normally starts pairing
 * (it sends a Security Request on connect). If nothing happened within this time we
 * start it ourselves. */
#define SECURITY_FALLBACK_MS 1500
/* After the ACK the camera hangs up by itself; if it does not, we do it. */
#define HANGUP_WAIT_MS 1500

/* ------------------------------------------------------------------------ */
/* Session state                                                             */
/* ------------------------------------------------------------------------ */

enum phase {
	PH_IDLE,
	PH_SCANNING,     /* looking for the camera's advertising */
	PH_CONNECTING,   /* connection request sent */
	PH_SECURING,     /* connected, waiting for pairing / encryption */
	PH_DISCOVERING,  /* MTU exchange + GATT discovery */
	PH_RECEIVING,    /* subscribed, picture is coming */
	PH_DONE,         /* finished (ok or not), tearing down */
};

static struct {
	volatile enum phase phase;
	camlink_result_t result;
	bool busy;
	bool pairing;                 /* accept any camera (true) / only the bonded one */
	bool have_bond;
	bt_addr_le_t bonded;          /* address of the paired camera */

	struct bt_conn *conn;

	uint16_t h_ctrl;              /* value handles found by discovery */
	uint16_t h_img;
	uint16_t svc_end;
	struct bt_gatt_discover_params disc;
	struct bt_gatt_subscribe_params sub;
	struct bt_gatt_discover_params sub_disc;   /* used by the CCC auto-discovery */
	struct bt_gatt_exchange_params mtu;

	/* picture being received */
	bool rx_active;
	bool rx_error;
	uint16_t rx_seq;
	uint32_t rx_next_log;
	int64_t rx_t0;
	uint8_t ack_value;
	enum camlink_kind kind;       /* what this session delivered */
	struct camlink_battery battery;

	struct k_sem finished;        /* given once when the session result is known */
	struct k_sem disconnected;    /* given when the link is down */
	struct k_work_delayable security_fallback;
	struct k_work ack_work;
} s;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

enum camlink_kind camlink_last_kind(void)
{
	return s.kind;
}

struct camlink_battery camlink_last_battery(void)
{
	return s.battery;
}

const char *camlink_result_str(camlink_result_t r)
{
	switch (r) {
	case CAMLINK_OK:        return "OK";
	case CAMLINK_NO_CAMERA: return "NO_CAMERA";
	case CAMLINK_TIMEOUT:   return "TIMEOUT";
	case CAMLINK_FAILED:    return "FAILED";
	case CAMLINK_BOND_LOST: return "BOND_LOST";
	case CAMLINK_TOO_BIG:   return "TOO_BIG";
	case CAMLINK_NO_BOND:   return "NO_BOND";
	case CAMLINK_BUSY:      return "BUSY";
	}
	return "?";
}

/* Record the result and release camlink_receive_image(). First call wins. */
static void finish(camlink_result_t result)
{
	if (s.phase == PH_DONE) {
		return;
	}
	s.phase = PH_DONE;
	s.result = result;
	k_sem_give(&s.finished);
}

static void bond_found_cb(const struct bt_bond_info *info, void *user_data)
{
	int *n = user_data;

	if (*n == 0) {
		s.bonded = info->addr;
		s.have_bond = true;
	}
	(*n)++;
}

static int scan_start(void);

static bool radio_on;
static bool auth_cb_registered;

/* ------------------------------------------------------------------------ */
/* Step: scan for the camera                                                 */
/* ------------------------------------------------------------------------ */

struct ad_ctx {
	bool name_match;
};

/* bt_data_parse() callback: is this advertising element our camera's name? */
static bool ad_cb(struct bt_data *data, void *user_data)
{
	struct ad_ctx *ctx = user_data;
	const size_t n = strlen(CONFIG_APP_CAMERA_NAME);

	if ((data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) &&
	    data->data_len >= n && memcmp(data->data, CONFIG_APP_CAMERA_NAME, n) == 0) {
		ctx->name_match = true;
		return false;   /* stop parsing */
	}
	return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	if (s.phase != PH_SCANNING) {
		return;
	}
	/* only connectable advertising can be answered with a connection */
	if (adv_type != BT_GAP_ADV_TYPE_ADV_IND && adv_type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	struct ad_ctx ctx = { .name_match = false };

	bt_data_parse(buf, ad_cb, &ctx);
	if (!ctx.name_match) {
		return;
	}

	char str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, str, sizeof(str));

	if (!s.pairing && !bt_addr_le_eq(addr, &s.bonded)) {
		LOG_DBG("camera name seen from %s, but it is not the paired one - ignored", str);
		return;
	}

	LOG_INF("camera found: %s, RSSI %d dBm -> connecting", str, rssi);
	s.phase = PH_CONNECTING;
	bt_le_scan_stop();

	int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				    BT_LE_CONN_PARAM(6, 12, 0, 400), &s.conn);
	if (err) {
		LOG_ERR("connection request failed: %d", err);
		s.phase = PH_SCANNING;
		scan_start();
	}
}

static int scan_start(void)
{
	/* passive scan, 100 % duty cycle (window = interval = 30 ms): the camera
	 * advertises every 20-30 ms and we want to hear the very first packet. */
	struct bt_le_scan_param param = {
		.type = BT_LE_SCAN_TYPE_PASSIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = 0x0030,
		.window = 0x0030,
	};
	int err = bt_le_scan_start(&param, scan_cb);

	if (err && err != -EALREADY) {
		LOG_ERR("scan start failed: %d", err);
		return err;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Steps: security -> MTU -> discovery -> subscribe                          */
/* ------------------------------------------------------------------------ */

static void discover_start(void);

static void mtu_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_exchange_params *params)
{
	LOG_INF("ATT MTU %u (status %u)", bt_gatt_get_mtu(conn), err);
	discover_start();
}

static void start_mtu_exchange(void)
{
	s.mtu.func = mtu_cb;
	int err = bt_gatt_exchange_mtu(s.conn, &s.mtu);

	if (err) {
		LOG_WRN("MTU exchange not started (%d), continuing", err);
		discover_start();
	}
}

/* Fallback: the camera did not start pairing, so we do. */
static void security_fallback_work(struct k_work *work)
{
	if (s.conn && s.phase == PH_SECURING) {
		LOG_INF("camera did not start pairing, requesting security ourselves");
		int err = bt_conn_set_security(s.conn, BT_SECURITY_L2);

		if (err) {
			LOG_ERR("bt_conn_set_security failed: %d", err);
		}
	}
}

static void subscribed_cb(struct bt_conn *conn, uint8_t err,
			  struct bt_gatt_subscribe_params *params)
{
	if (err) {
		LOG_ERR("subscribing to the image characteristic failed: %u", err);
		finish(CAMLINK_FAILED);
		return;
	}
	s.phase = PH_RECEIVING;
	LOG_INF("subscribed - waiting for the message from the camera");
	/* tell the camera we really listen now (it must not send before this) */
	s.ack_value = CTRL_READY;
	k_work_submit(&s.ack_work);
}

static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length);

static void subscribe_start(void)
{
	memset(&s.sub, 0, sizeof(s.sub));
	memset(&s.sub_disc, 0, sizeof(s.sub_disc));
	s.sub.notify = notify_cb;
	s.sub.subscribe = subscribed_cb;
	s.sub.value = BT_GATT_CCC_NOTIFY;
	s.sub.value_handle = s.h_img;
	s.sub.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;   /* find the CCC descriptor for us */
	s.sub.end_handle = s.svc_end;
	s.sub.disc_params = &s.sub_disc;
	/* volatile: not remembered across connections, we subscribe again every time */
	atomic_set_bit(s.sub.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

	int err = bt_gatt_subscribe(s.conn, &s.sub);

	if (err && err != -EALREADY) {
		LOG_ERR("bt_gatt_subscribe failed: %d", err);
		finish(CAMLINK_FAILED);
	}
}

/* Called for every result of the two discovery runs (service, then its characteristics). */
static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	if (!attr) {
		/* end of this run */
		if (params->type == BT_GATT_DISCOVER_PRIMARY) {
			LOG_ERR("camera service not found on this device");
			finish(CAMLINK_FAILED);
		} else if (!s.h_img || !s.h_ctrl) {
			LOG_ERR("camera characteristics missing (image %u, control %u)", s.h_img,
				s.h_ctrl);
			finish(CAMLINK_FAILED);
		} else {
			LOG_INF("service found: image handle %u, control handle %u", s.h_img,
				s.h_ctrl);
			subscribe_start();
		}
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		const struct bt_gatt_service_val *svc = attr->user_data;

		s.svc_end = svc->end_handle;
		params->uuid = NULL;                       /* all characteristics ... */
		params->start_handle = attr->handle + 1;   /* ... inside this service */
		params->end_handle = svc->end_handle;
		params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
		int err = bt_gatt_discover(conn, params);

		if (err) {
			LOG_ERR("characteristic discovery failed: %d", err);
			finish(CAMLINK_FAILED);
		}
		return BT_GATT_ITER_STOP;
	}

	/* BT_GATT_DISCOVER_CHARACTERISTIC */
	const struct bt_gatt_chrc *chrc = attr->user_data;

	if (bt_uuid_cmp(chrc->uuid, &uuid_img.uuid) == 0) {
		s.h_img = chrc->value_handle;
	} else if (bt_uuid_cmp(chrc->uuid, &uuid_ctrl.uuid) == 0) {
		s.h_ctrl = chrc->value_handle;
	}
	return BT_GATT_ITER_CONTINUE;
}

static void discover_start(void)
{
	s.phase = PH_DISCOVERING;
	s.h_img = 0;
	s.h_ctrl = 0;
	memset(&s.disc, 0, sizeof(s.disc));
	s.disc.uuid = &uuid_svc.uuid;
	s.disc.func = discover_cb;
	s.disc.start_handle = 0x0001;
	s.disc.end_handle = 0xffff;
	s.disc.type = BT_GATT_DISCOVER_PRIMARY;

	int err = bt_gatt_discover(s.conn, &s.disc);

	if (err) {
		LOG_ERR("service discovery failed: %d", err);
		finish(CAMLINK_FAILED);
	}
}

/* ------------------------------------------------------------------------ */
/* Step: receive the picture                                                 */
/* ------------------------------------------------------------------------ */

/* System work queue: write the ACK / NAK (not from the Bluetooth thread). */
static void ack_work_fn(struct k_work *work)
{
	struct bt_conn *conn = s.conn;
	uint8_t v = s.ack_value;

	if (!conn) {
		return;
	}
	int err = bt_gatt_write_without_response(conn, s.h_ctrl, &v, 1, false);

	if (err) {
		LOG_ERR("could not write control byte 0x%02x: %d", v, err);
		finish(CAMLINK_FAILED);
		return;
	}
	if (v == CTRL_READY) {
		LOG_INF("READY sent to the camera");
		return;
	}
	LOG_INF("%s sent to the camera", v == CTRL_ACK ? "ACK" : "NAK (resend requested)");
	if (v == CTRL_ACK) {
		finish(CAMLINK_OK);
	}
}

static void rx_start(const uint8_t *d, uint16_t len)
{
	if (len < 9) {
		LOG_WRN("short START packet (%u bytes)", len);
		return;
	}
	uint32_t total = sys_get_le32(&d[1]);
	uint16_t w = sys_get_le16(&d[5]);
	uint16_t h = sys_get_le16(&d[7]);

	uint8_t flags = len >= 11 ? d[9] : 0;
	uint8_t faces = len >= 11 ? d[10] : 0;
	uint8_t humans = len >= 12 ? d[11] : 0;
	enum image_status status = (flags & IMG_FLAG_FACE) ? IMAGE_FACE :
				   (flags & IMG_FLAG_HUMAN) ? IMAGE_HUMAN : IMAGE_NO_HUMAN;

	if (image_store_begin(total, w, h, status, humans, faces) != 0) {
		LOG_ERR("picture of %u bytes does not fit into the %u byte buffer", total,
			(unsigned int)CONFIG_APP_IMAGE_BUF_SIZE);
		finish(CAMLINK_TOO_BIG);
		return;
	}
	s.rx_active = true;
	s.rx_error = false;
	s.rx_seq = 0;
	s.rx_next_log = CONFIG_APP_PROGRESS_LOG_BYTES;
	s.rx_t0 = k_uptime_get();
	LOG_INF("incoming picture %ux%u, %u bytes, status: %s", w, h, total,
		image_status_str(status));
}

static void rx_data(const uint8_t *d, uint16_t len)
{
	if (!s.rx_active || len < 3) {
		return;
	}
	uint16_t seq = sys_get_le16(&d[1]);

	if (seq != s.rx_seq) {
		LOG_WRN("packet loss: expected #%u, got #%u", s.rx_seq, seq);
		s.rx_error = true;
	}
	s.rx_seq = seq + 1;

	if (image_store_append(&d[3], len - 3) != 0) {
		LOG_ERR("more data than announced");
		s.rx_error = true;
		return;
	}
	if (image_store_received() >= s.rx_next_log) {
		LOG_INF("saved %u / %u bytes", image_store_received(), image_store_expected());
		s.rx_next_log += CONFIG_APP_PROGRESS_LOG_BYTES;
	}
}

static void rx_end(const uint8_t *d, uint16_t len)
{
	if (!s.rx_active || len < 5) {
		return;
	}
	uint32_t crc_rx = sys_get_le32(&d[1]);
	uint32_t got = image_store_received();
	uint32_t want = image_store_expected();
	uint32_t crc = image_store_crc();
	int64_t ms = k_uptime_get() - s.rx_t0;

	s.rx_active = false;
	if (!s.rx_error && got == want && crc == crc_rx) {
		image_store_commit();
		LOG_INF("TRANSMISSION END: %u bytes saved in RAM, CRC32 %08x OK, %lld ms (%u B/s)",
			got, crc, (long long)ms, ms > 0 ? (unsigned int)((int64_t)got * 1000 / ms) : 0);
		s.ack_value = CTRL_ACK;
		s.kind = CAMLINK_KIND_IMAGE;
	} else {
		LOG_ERR("TRANSMISSION END with errors: saved %u of %u bytes, CRC %08x vs %08x%s",
			got, want, crc, crc_rx, s.rx_error ? ", packets lost" : "");
		image_store_abort();
		s.ack_value = CTRL_NAK;
	}
	k_work_submit(&s.ack_work);
}

static void rx_battery(const uint8_t *d, uint16_t len)
{
	if (len < 6) {
		LOG_WRN("short battery packet (%u bytes)", len);
		return;
	}
	s.battery.percent = d[1];
	s.battery.battery_mv = sys_get_le16(&d[2]);
	s.battery.adc_mv = sys_get_le16(&d[4]);
	s.kind = CAMLINK_KIND_BATTERY;
	LOG_INF("BATTERY: %u %% (%u mV battery, %u mV at the camera's ADC pin)", s.battery.percent,
		s.battery.battery_mv, s.battery.adc_mv);
	s.ack_value = CTRL_ACK;
	k_work_submit(&s.ack_work);
}

/* GATT notification from the Image characteristic (Bluetooth RX thread: keep it short). */
static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	if (!data) {
		LOG_INF("unsubscribed");
		return BT_GATT_ITER_STOP;
	}
	const uint8_t *d = data;

	if (length == 0) {
		return BT_GATT_ITER_CONTINUE;
	}
	switch (d[0]) {
	case PKT_START: rx_start(d, length); break;
	case PKT_DATA:  rx_data(d, length);  break;
	case PKT_END:   rx_end(d, length);   break;
	case PKT_BATTERY: rx_battery(d, length); break;
	default:
		LOG_WRN("unknown packet type 0x%02x", d[0]);
	}
	return BT_GATT_ITER_CONTINUE;
}

/* ------------------------------------------------------------------------ */
/* Connection callbacks                                                      */
/* ------------------------------------------------------------------------ */

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (conn != s.conn) {
		return;
	}
	if (err) {
		LOG_WRN("connection failed: 0x%02x", err);
		bt_conn_unref(s.conn);
		s.conn = NULL;
		k_sem_give(&s.disconnected);
		if (s.phase != PH_DONE) {
			s.phase = PH_SCANNING;
			scan_start();       /* try again until the session times out */
		}
		return;
	}
	LOG_INF("connected, waiting for pairing / encryption");
	s.phase = PH_SECURING;
	k_work_reschedule(&s.security_fallback, K_MSEC(SECURITY_FALLBACK_MS));
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	if (conn != s.conn) {
		return;
	}
	LOG_INF("disconnected (reason 0x%02x)", reason);
	k_work_cancel_delayable(&s.security_fallback);
	bt_conn_unref(s.conn);
	s.conn = NULL;
	finish(CAMLINK_FAILED);          /* no effect if the picture was already acknowledged */
	k_sem_give(&s.disconnected);
}

static void security_changed_cb(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	if (conn != s.conn || s.phase != PH_SECURING) {
		return;
	}
	k_work_cancel_delayable(&s.security_fallback);

	if (err) {
		LOG_ERR("security failed: error %d (level %d)", err, level);
		if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
			/* the camera has no key for us any more (it was reset) */
			if (IS_ENABLED(CONFIG_APP_AUTO_REPAIR)) {
				LOG_WRN("camera lost the bond: deleting ours");
				bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn));
			}
			finish(CAMLINK_BOND_LOST);
		} else {
			finish(CAMLINK_FAILED);
		}
		return;
	}
	LOG_INF("link encrypted (security level %d)", level);
	start_mtu_exchange();
}

static void pairing_complete_cb(struct bt_conn *conn, bool bonded)
{
	LOG_INF("pairing complete, bonded: %s", bonded ? "yes (stored in flash)" : "no");
}

static void pairing_failed_cb(struct bt_conn *conn, enum bt_security_err reason)
{
	LOG_ERR("pairing failed, reason %d", reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
	.security_changed = security_changed_cb,
};

static struct bt_conn_auth_info_cb auth_info_callbacks = {
	.pairing_complete = pairing_complete_cb,
	.pairing_failed = pairing_failed_cb,
};

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void camlink_init(void)
{
	k_sem_init(&s.finished, 0, 1);
	k_sem_init(&s.disconnected, 0, 1);
	k_work_init_delayable(&s.security_fallback, security_fallback_work);
	k_work_init(&s.ack_work, ack_work_fn);
}

int camlink_radio_on(void)
{
	if (radio_on) {
		return 0;
	}
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return err;
	}
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();   /* restores the bonds */
	}
	if (!auth_cb_registered) {
		bt_conn_auth_info_cb_register(&auth_info_callbacks);
		auth_cb_registered = true;
	}
	radio_on = true;

	bt_addr_le_t id[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(id);
	char str[BT_ADDR_LE_STR_LEN] = "?";

	bt_id_get(id, &count);
	if (count > 0) {
		bt_addr_le_to_str(&id[0], str, sizeof(str));
	}
	LOG_INF("Bluetooth ON, own address %s, %d paired camera(s)", str, camlink_bond_count());
	return 0;
}

void camlink_radio_off(void)
{
	if (!radio_on) {
		return;
	}
	int err = bt_disable();

	if (err) {
		LOG_WRN("bt_disable failed: %d (Bluetooth stays on)", err);
		return;
	}
	radio_on = false;
	LOG_INF("Bluetooth OFF");
}

int camlink_bond_count(void)
{
	int n = 0;

	bt_foreach_bond(BT_ID_DEFAULT, bond_found_cb, &n);
	return n;
}

void camlink_forget_all(void)
{
	int err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

	LOG_WRN("all paired cameras forgotten (%d)", err);
}

camlink_result_t camlink_receive_image(bool pairing, uint32_t find_timeout_ms)
{
	if (s.busy) {
		return CAMLINK_BUSY;
	}

	/* fresh session state */
	s.have_bond = false;
	int n = 0;

	bt_foreach_bond(BT_ID_DEFAULT, bond_found_cb, &n);
	if (!pairing && !s.have_bond) {
		LOG_ERR("no paired camera - use pairing mode");
		return CAMLINK_NO_BOND;
	}
	s.busy = true;
	s.pairing = pairing;
	s.result = CAMLINK_FAILED;
	s.kind = CAMLINK_KIND_NONE;
	s.rx_active = false;
	s.h_img = s.h_ctrl = 0;
	k_sem_reset(&s.finished);
	k_sem_reset(&s.disconnected);

	s.phase = PH_SCANNING;
	if (scan_start() != 0) {
		s.phase = PH_IDLE;
		s.busy = false;
		return CAMLINK_FAILED;
	}
	LOG_INF("listening for \"%s\" (%s), up to %u ms", CONFIG_APP_CAMERA_NAME,
		pairing ? "pairing mode: any camera" : "paired camera only", find_timeout_ms);

	/* 1) wait for the camera to appear and the session to finish */
	int rc = k_sem_take(&s.finished, K_MSEC(find_timeout_ms));

	/* 2) found but not finished: give the connection + pairing + transfer more time */
	if (rc == -EAGAIN && s.phase > PH_SCANNING && s.phase != PH_DONE) {
		LOG_INF("camera found, waiting up to %d s for the transfer",
			CONFIG_APP_TRANSFER_TIMEOUT_S);
		rc = k_sem_take(&s.finished, K_SECONDS(CONFIG_APP_TRANSFER_TIMEOUT_S));
	}
	if (rc == -EAGAIN) {
		camlink_result_t r = (s.phase <= PH_SCANNING) ? CAMLINK_NO_CAMERA : CAMLINK_TIMEOUT;

		finish(r);
		LOG_WRN("session timed out (%s)", camlink_result_str(r));
	}

	/* teardown */
	s.phase = PH_DONE;
	bt_le_scan_stop();
	k_work_cancel_delayable(&s.security_fallback);

	if (s.conn) {
		/* After the ACK the camera hangs up by itself. Give it a moment, then do it. */
		if (k_sem_take(&s.disconnected, K_MSEC(HANGUP_WAIT_MS)) != 0 && s.conn) {
			bt_conn_disconnect(s.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			k_sem_take(&s.disconnected, K_SECONDS(3));
		}
	}

	camlink_result_t result = s.result;

	LOG_INF("session over: %s", camlink_result_str(result));
	s.phase = PH_IDLE;
	s.busy = false;
	return result;
}
