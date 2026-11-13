/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lora_transport.h"
#if defined(CONFIG_APP_LORA_DEMO)
#include "message_ids.h"
#endif

LOG_MODULE_REGISTER(master_node, LOG_LEVEL_INF);

int main(void)
{
	/* Allow USB enumeration; do not block forever waiting for a terminal's DTR. */
	k_sleep(K_SECONDS(2));
#if defined(CONFIG_APP_LORA_DEMO)
	int ret = bs_message_ids_init();

	if (ret < 0) {
		LOG_ERR("Persistent message IDs unavailable: %d; refusing to reuse IDs", ret);
		return ret;
	}
	ret = bs_lora_init();
	if (ret < 0) {
		LOG_ERR("LoRa initialization failed: %d", ret);
		return ret;
	}
	static const uint8_t test_event[] = "{\"event\":\"test_entry\",\"test\":true}";

	while (true) {
		uint32_t message_id;
		struct bs_tx_result result = {0};

		ret = bs_message_id_next(&message_id);
		if (ret < 0) {
			LOG_ERR("Cannot reserve message ID: %d; sender stopped", ret);
			return ret;
		}
		LOG_INF("ENTRY device=%u message=%u bytes=%u fragments=%u",
			CONFIG_APP_DEVICE_ID, message_id, (unsigned)(sizeof(test_event) - 1U),
			(unsigned)DIV_ROUND_UP(sizeof(test_event) - 1U, BS_PART_DEFAULT));
		ret = bs_lora_send_message(BS_ENTRY, CONFIG_APP_DEVICE_ID, message_id,
					  test_event, sizeof(test_event) - 1U, &result);
		if (ret == 0) {
			LOG_INF("Delivered device=%u message=%u DATA=%u END=%u",
				CONFIG_APP_DEVICE_ID, message_id, result.data_attempts,
				result.end_attempts);
		} else {
			LOG_WRN("Not confirmed device=%u message=%u error=%d DATA=%u END=%u",
				CONFIG_APP_DEVICE_ID, message_id, ret, result.data_attempts,
				result.end_attempts);
		}
		/* New ID on the next iteration; internal retries always retained this ID. */
		k_sleep(K_SECONDS(CONFIG_APP_DEMO_INTERVAL_SECONDS));
	}
#else
	LOG_INF("LoRa demo disabled; integrate bs_lora_init/bs_lora_send_message from the application");
	return 0;
#endif
}
