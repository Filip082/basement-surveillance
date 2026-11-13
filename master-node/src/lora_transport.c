/* SPDX-License-Identifier: Apache-2.0 */
#include "lora_transport.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
/* Existing exports from Zephyr 4.4.2 drivers/lora/loramac-node/sx126x.c.
 * Keep this exact backend dependency isolated; no application SPI/GPIO ownership.
 */
extern void SX126xWriteRegisters(uint16_t address, uint8_t *buffer, uint16_t size);
extern void SX126xReadRegisters(uint16_t address, uint8_t *buffer, uint16_t size);

LOG_MODULE_REGISTER(bs_lora, LOG_LEVEL_INF);

BUILD_ASSERT(IS_ENABLED(CONFIG_LORA_MODULE_BACKEND_LORAMAC_NODE),
	     "The custom sync-word adapter requires the pinned LoRaMac-node backend");
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_ALIAS(lora0), semtech_sx1262), "lora0 must be an SX1262");

#if CONFIG_APP_LORA_BANDWIDTH == 125000
#define APP_BANDWIDTH BW_125_KHZ
#elif CONFIG_APP_LORA_BANDWIDTH == 250000
#define APP_BANDWIDTH BW_250_KHZ
#elif CONFIG_APP_LORA_BANDWIDTH == 500000
#define APP_BANDWIDTH BW_500_KHZ
#else
#error "APP_LORA_BANDWIDTH must be 125000, 250000 or 500000"
#endif

static const struct device *const radio = DEVICE_DT_GET(DT_ALIAS(lora0));
static struct lora_modem_config modem = {
	.frequency = CONFIG_APP_LORA_FREQUENCY,
	.bandwidth = APP_BANDWIDTH,
	.datarate = CONFIG_APP_LORA_SF,
	.coding_rate = CR_4_5,
	.preamble_len = 12,
	.tx_power = CONFIG_APP_LORA_TX_POWER,
	.tx = true,
	.iq_inverted = false,
	.public_network = false,
	.packet_crc_disable = false,
};
static K_MUTEX_DEFINE(session_lock);
static uint64_t next_tx_ms;
static uint32_t feedback_timeout_ms;
static uint16_t part_size;
static bool initialized;

/* lora_config() puts the chip into warm sleep. The pinned register accessors wake it
 * through the driver's own SPI/GPIO implementation and retain these settings across
 * subsequent warm sleeps. This is the ONLY use of a backend-specific API.
 * Zephyr 4.4.2's public bool public_network cannot represent raw 0x0741.
 */
static int configure(bool tx)
{
	modem.tx = tx;
	int ret = lora_config(radio, &modem);

	if (ret < 0) {
		return ret;
	}
	uint8_t desired[] = {0x07, 0x41};
	uint8_t actual[2] = {0};

	SX126xWriteRegisters(0x0740, desired, sizeof(desired));
	SX126xReadRegisters(0x0740, actual, sizeof(actual));
	if (memcmp(desired, actual, sizeof(actual)) != 0) {
		LOG_ERR("Sync word readback failed: %02x%02x", actual[0], actual[1]);
		return -EIO;
	}
	return 0;
}

static uint64_t now_ms(void *context)
{
	ARG_UNUSED(context);
	return (uint64_t)k_uptime_get();
}

static int send_frame(void *context, const uint8_t *frame, size_t size)
{
	ARG_UNUSED(context);
	if (frame == NULL || size == 0 || size > BS_RADIO_MAX) {
		return -EINVAL;
	}
	uint64_t now = now_ms(NULL);

	if (now < next_tx_ms) {
		LOG_INF("Duty-cycle delay: %llu ms", (unsigned long long)(next_tx_ms - now));
		k_msleep((int64_t)(next_tx_ms - now));
	}
	int ret = configure(true);

	if (ret < 0) {
		return ret;
	}
	/* The driver queries its actual configured modulation. Round up and include
	 * oscillator/PA/timing margin. Failed transmissions consume the same allowance.
	 */
	uint32_t airtime_ms = lora_airtime(radio, (uint32_t)size) + 5U;
	uint8_t packet[BS_RADIO_MAX];

	memcpy(packet, frame, size);
	ret = lora_send(radio, packet, (uint32_t)size);
	/* Charge from completion, which is conservative if the call returned early.
	 * Strict spacing bounds TX duty without keeping an hourly packet ledger.
	 */
	next_tx_ms = now_ms(NULL) + bs_tx_spacing_ms(airtime_ms, CONFIG_APP_LORA_DUTY_PERMILLE) -
		     airtime_ms;
	return ret;
}

static int receive_frame(void *context, uint8_t *frame, size_t capacity, uint32_t timeout_ms)
{
	ARG_UNUSED(context);
	if (capacity == 0 || capacity > BS_RADIO_MAX || timeout_ms == 0) {
		return -EINVAL;
	}
	uint64_t deadline = now_ms(NULL) + timeout_ms;
	int ret = configure(false);

	if (ret < 0) {
		return ret;
	}
	uint64_t now = now_ms(NULL);

	if (now >= deadline) {
		return -ETIMEDOUT;
	}
	int16_t rssi;
	int8_t snr;

	ret = lora_recv(radio, frame, (uint8_t)capacity, K_MSEC(deadline - now), &rssi, &snr);
	/* The pinned backend returns -EAGAIN when k_poll times out. Each subsequent
	 * operation reconfigures the released modem, recovering ordinary TX/RX timeouts.
	 */
	if (ret > 0) {
		LOG_DBG("RX %d bytes RSSI=%d SNR=%d", ret, rssi, snr);
	}
	return ret;
}

static void session_event(void *context, enum bs_tx_event event, int detail)
{
	ARG_UNUSED(context);
	switch (event) {
	case BS_TX_END:
		LOG_INF("END attempt %d/%d", detail, CONFIG_APP_LORA_END_ATTEMPTS);
		break;
	case BS_TX_ACK:
		LOG_INF("Matching ACK received (round %d)", detail);
		break;
	case BS_TX_NACK:
		LOG_INF("Matching NACK requests %d fragments", detail);
		break;
	case BS_TX_MISSING:
		LOG_INF("Retransmitting missing fragment %d", detail);
		break;
	case BS_TX_TIMEOUT:
		LOG_WRN("Feedback timeout after END round %d; retry END only", detail);
		break;
	case BS_TX_IO_ERROR:
		LOG_WRN("Packet TX failed: %d; bounded retry", detail);
		break;
	case BS_TX_IGNORED:
		LOG_DBG("Ignored malformed/unrelated feedback (%d bytes)", detail);
		break;
	}
}

int bs_lora_init(void)
{
	if (initialized) {
		return -EALREADY;
	}
	if (!device_is_ready(radio)) {
		LOG_ERR("lora0 is not ready; check the overlay, BUSY, DIO1 and power");
		return -ENODEV;
	}
	if (CONFIG_APP_LORA_FREQUENCY - CONFIG_APP_LORA_BANDWIDTH / 2 < 868000000 ||
	    CONFIG_APP_LORA_FREQUENCY + CONFIG_APP_LORA_BANDWIDTH / 2 > 868600000) {
		LOG_ERR("Configured channel does not fit the 868.0..868.6 MHz duty profile");
		return -EINVAL;
	}
	int ret = configure(true);

	if (ret < 0) {
		return ret;
	}
	/* On reboot there is no trustworthy elapsed wall time. Wait a full maximum-
	 * packet spacing so cycling power cannot shorten the previous TX's off-time.
	 */
	/* The pinned backend gives Radio.SetTxConfig a 4000 ms software timeout.
	 * Keep DATA below 3750 ms at slow SFs rather than changing private timer state.
	 * Fragment size is not part of the PHY settings and is legal in protocol v1.
	 */
	part_size = BS_PART_DEFAULT;
	while (part_size > 1U && lora_airtime(radio, BS_HEADER_SIZE + part_size) > 3750U) {
		--part_size;
	}
	if (lora_airtime(radio, BS_HEADER_SIZE + part_size) > 3750U) {
		return -ENOTSUP;
	}
	uint32_t max_airtime = lora_airtime(radio, BS_RADIO_MAX) + 5U;

	next_tx_ms = now_ms(NULL) + bs_tx_spacing_ms(max_airtime, CONFIG_APP_LORA_DUTY_PERMILLE);
	/* A preceding maximum-size NACK can defer the router's next feedback. Include
	 * its default 1% spacing, quiet time, response airtime and scheduling margin.
	 */
	uint32_t reply_airtime = lora_airtime(radio, BS_CONTROL_MAX) + 5U;
	uint64_t reply_window = bs_tx_spacing_ms(reply_airtime, CONFIG_APP_ROUTER_DUTY_PERMILLE) +
				 reply_airtime + CONFIG_APP_ROUTER_QUIET_MS + 2000U;

	if (reply_window > UINT32_MAX) {
		return -ERANGE;
	}
	feedback_timeout_ms = MAX(CONFIG_APP_LORA_FEEDBACK_TIMEOUT_MS, (uint32_t)reply_window);
	initialized = true;
	LOG_INF("SX1262 %u Hz SF%d BW%u CR4/5 sync=0741 preamble=12 CRC=on",
		CONFIG_APP_LORA_FREQUENCY, CONFIG_APP_LORA_SF, CONFIG_APP_LORA_BANDWIDTH);
	LOG_INF("TX duty=%d/1000; feedback window=%u ms; boot guard=%u ms; DATA chunk=%u",
		CONFIG_APP_LORA_DUTY_PERMILLE, feedback_timeout_ms,
		(uint32_t)(next_tx_ms - now_ms(NULL)), part_size);
	return 0;
}

int bs_lora_send_message(uint8_t kind, uint16_t device_id, uint32_t message_id,
			 const uint8_t *data, size_t size, struct bs_tx_result *result)
{
	if (!initialized) {
		return -ENODEV;
	}
	if (k_mutex_lock(&session_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	const struct bs_tx_io io = {
		.send = send_frame, .recv = receive_frame, .now_ms = now_ms, .event = session_event,
	};
	const struct bs_tx_options options = {
		.feedback_timeout_ms = feedback_timeout_ms,
		.part_size = part_size,
		.max_end_attempts = CONFIG_APP_LORA_END_ATTEMPTS,
		.max_packet_attempts = CONFIG_APP_LORA_PACKET_ATTEMPTS,
	};
	int ret = bs_tx_send(&io, &options, kind, device_id, message_id, data, size, result);

	k_mutex_unlock(&session_lock);
	return ret;
}
