/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Example main(): sleep until a trigger input is activated, then ask the camera node for
 * a picture (Button 1) or for its battery level (Button 2). Everything camera related is behind camera_node.h
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
BUILD_ASSERT(DT_NODE_HAS_PROP(DT_PATH(zephyr_user), battery_trigger_gpios),
	     "the board overlay must define zephyr,user battery-trigger-gpios");

/* trigger = "take a picture" (Button 1), battery trigger = "report the battery" (Button 2) */
static const struct gpio_dt_spec trigger = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), trigger_gpios);
static const struct gpio_dt_spec battery_trigger =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), battery_trigger_gpios);
static struct gpio_callback trigger_cb_data;
static struct gpio_callback battery_cb_data;
static K_SEM_DEFINE(trigger_sem, 0, 1);

#define REQ_IMAGE   0   /* bit numbers in `requests` */
#define REQ_BATTERY 1
static atomic_t requests;

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
	atomic_set_bit(&requests, REQ_IMAGE);
	k_sem_give(&trigger_sem);
}

/* Interrupt: battery button. Debounced like the picture trigger. */
static void battery_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	static int64_t last_ms;
	int64_t now = k_uptime_get();

	if (now - last_ms < TRIGGER_DEBOUNCE_MS) {
		return;
	}
	last_ms = now;
	atomic_set_bit(&requests, REQ_BATTERY);
	k_sem_give(&trigger_sem);
}

static int input_init(const struct gpio_dt_spec *spec, struct gpio_callback *cb,
		      gpio_callback_handler_t isr)
{
	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("input GPIO controller not ready");
		return -ENODEV;
	}
	int err = gpio_pin_configure_dt(spec, GPIO_INPUT);

	if (err) {
		return err;
	}
	gpio_init_callback(cb, isr, BIT(spec->pin));
	err = gpio_add_callback(spec->port, cb);
	if (err) {
		return err;
	}
	return gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_TO_ACTIVE);
}

int main(void)
{
	LOG_INF("master-node up on %s", CONFIG_BOARD_TARGET);

	if (camera_node_init() || input_init(&trigger, &trigger_cb_data, trigger_isr) ||
	    input_init(&battery_trigger, &battery_cb_data, battery_isr)) {
		LOG_ERR("setup failed - check the board overlay");
		return 0;
	}

	for (;;) {
		LOG_INF("sleeping until the trigger input is activated");
		k_sem_take(&trigger_sem, K_FOREVER);

		if (atomic_test_and_clear_bit(&requests, REQ_IMAGE) && take_image()) {
			const struct stored_image *img = get_image();

			/* >>> picture is in RAM: img->data, img->len, img->status. Hand it to the
			 *     next stage here (LoRa, ...). Valid until the next take_image(). <<< */
			LOG_INF("image ready: %u bytes, %s", img->len, image_status_str(img->status));
		}

		if (atomic_test_and_clear_bit(&requests, REQ_BATTERY)) {
			struct battery_info bat;

			if (read_battery(&bat)) {
				LOG_INF("camera node battery: %u %% (%u mV)", bat.percent, bat.battery_mv);
			}
		}
	}
	return 0;
}
