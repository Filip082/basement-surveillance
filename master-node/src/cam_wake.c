/* SPDX-License-Identifier: Apache-2.0 */

#include "cam_wake.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(cam_wake, LOG_LEVEL_DBG);

BUILD_ASSERT(DT_NODE_HAS_PROP(DT_PATH(zephyr_user), cam_wake_gpios),
	     "the board overlay must define zephyr,user cam-wake-gpios");

static const struct gpio_dt_spec wake = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), cam_wake_gpios);

int cam_wake_init(void)
{
	if (!gpio_is_ready_dt(&wake)) {
		LOG_ERR("wake GPIO controller not ready");
		return -ENODEV;
	}
	int err = gpio_pin_configure_dt(&wake, GPIO_OUTPUT_INACTIVE);

	if (err) {
		LOG_ERR("wake GPIO configure failed: %d", err);
		return err;
	}
	LOG_INF("camera wake line ready (%s pin %u, active %s)", wake.port->name, wake.pin,
		(wake.dt_flags & GPIO_ACTIVE_LOW) ? "LOW" : "HIGH");
	return 0;
}

void cam_wake_pulse(void)
{
	LOG_INF("waking the camera node (%d ms pulse)", CONFIG_APP_CAM_WAKE_PULSE_MS);
	gpio_pin_set_dt(&wake, 1);
	k_msleep(CONFIG_APP_CAM_WAKE_PULSE_MS);
	gpio_pin_set_dt(&wake, 0);
}
