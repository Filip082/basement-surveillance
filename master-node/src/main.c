/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Example main(): sleep until the trigger input is activated, then ask the camera
 * node for a picture. Everything camera related is behind camera_node.h
 * (take_image() / get_image()); the rest of the product plugs in where marked.
 *
 * The trigger is Button 1 on the nRF5340 DK; in the final product it is a line from
 * another MCU - only the devicetree overlay changes. Between triggers the CPU
 * sleeps in the Zephyr idle thread (System ON, RAM kept).
 */

#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "camera_node.h"

LOG_MODULE_REGISTER(master_node, LOG_LEVEL_DBG);

BUILD_ASSERT(DT_NODE_HAS_PROP(DT_PATH(zephyr_user), trigger_gpios),
	     "the board overlay must define zephyr,user trigger-gpios");

static const struct gpio_dt_spec trigger = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), trigger_gpios);
static struct gpio_callback trigger_cb_data;
static K_SEM_DEFINE(trigger_sem, 0, 1);

#define TRIGGER_DEBOUNCE_MS 300

/* Interrupt: a trigger edge. Debounced; wakes the main thread. */
static void trigger_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	static int64_t last_ms;
	int64_t now = k_uptime_get();

	if (now - last_ms < TRIGGER_DEBOUNCE_MS) {
		return;
	}
	last_ms = now;
	k_sem_give(&trigger_sem);
}

static int trigger_init(void)
{
	if (!gpio_is_ready_dt(&trigger)) {
		LOG_ERR("trigger GPIO controller not ready");
		return -ENODEV;
	}
	int err = gpio_pin_configure_dt(&trigger, GPIO_INPUT);

	if (err) {
		return err;
	}
	gpio_init_callback(&trigger_cb_data, trigger_isr, BIT(trigger.pin));
	err = gpio_add_callback(trigger.port, &trigger_cb_data);
	if (err) {
		return err;
	}
	return gpio_pin_interrupt_configure_dt(&trigger, GPIO_INT_EDGE_TO_ACTIVE);
}

int main(void)
{
	LOG_INF("master-node up on %s", CONFIG_BOARD_TARGET);

	if (camera_node_init() || trigger_init()) {
		LOG_ERR("setup failed - check the board overlay");
		return 0;
	}

	for (;;) {
		LOG_INF("sleeping until the trigger input is activated");
		k_sem_take(&trigger_sem, K_FOREVER);

		if (take_image()) {
			const struct stored_image *img = get_image();

			/* >>> picture is in RAM: img->data, img->len. Hand it to the next
			 *     stage here (LoRa, ...). Valid until the next take_image(). <<< */
			LOG_INF("image ready: %u bytes", img->len);
		}
	}
	return 0;
}
