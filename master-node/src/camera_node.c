/* SPDX-License-Identifier: Apache-2.0 */

#include "camera_node.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cam_wake.h"
#include "camera_link.h"

LOG_MODULE_REGISTER(camera_node, LOG_LEVEL_DBG);

static struct stored_image current;
static bool have_image;

int camera_node_init(void)
{
	camlink_init();
	return cam_wake_init();
}

bool take_image(void)
{
	have_image = false;

	if (camlink_radio_on() != 0) {                                  /* Bluetooth on */
		return false;
	}

	bool paired = camlink_bond_count() > 0;

	LOG_INF("take_image: %s", paired ? "paired camera" : "no camera paired yet: pairing mode");

	cam_wake_pulse(CONFIG_APP_IMAGE_WAKE_PULSE_MS);                 /* wake the camera: short pulse */
	camlink_result_t r = camlink_receive_image(!paired, CONFIG_APP_SESSION_TIMEOUT_S * 1000);

	if (r == CAMLINK_BOND_LOST && IS_ENABLED(CONFIG_APP_AUTO_REPAIR)) {
		LOG_WRN("camera forgot us: waking it again and pairing from scratch");
		cam_wake_pulse(CONFIG_APP_IMAGE_WAKE_PULSE_MS);
		r = camlink_receive_image(true, CONFIG_APP_SESSION_TIMEOUT_S * 1000);
	}

	camlink_radio_off();                                            /* Bluetooth off */

	if (r == CAMLINK_OK && camlink_last_kind() != CAMLINK_KIND_IMAGE) {
		LOG_WRN("take_image: the camera sent something else (not a picture)");
		r = CAMLINK_FAILED;
	}

	if (r == CAMLINK_OK && image_store_get(&current)) {
		have_image = true;
		LOG_INF("take_image: picture ready, %u bytes (%ux%u), status: %s", current.len,
			current.width, current.height, image_status_str(current.status));
	} else {
		LOG_WRN("take_image: no picture (%s)", camlink_result_str(r));
	}
	return have_image;
}

bool read_battery(struct battery_info *out)
{
	bool ok = false;

	if (camlink_radio_on() != 0) {
		return false;
	}
	if (camlink_bond_count() == 0) {
		LOG_ERR("read_battery: no camera paired yet - call take_image() once first");
		camlink_radio_off();
		return false;
	}

	LOG_INF("read_battery: asking the camera node");
	cam_wake_pulse(CONFIG_APP_BATTERY_WAKE_PULSE_MS);                 /* long pulse = battery */
	camlink_result_t r = camlink_receive_image(false, CONFIG_APP_SESSION_TIMEOUT_S * 1000);

	camlink_radio_off();

	if (r == CAMLINK_OK && camlink_last_kind() == CAMLINK_KIND_BATTERY) {
		struct camlink_battery b = camlink_last_battery();

		if (out) {
			out->percent = b.percent;
			out->battery_mv = b.battery_mv;
			out->adc_mv = b.adc_mv;
		}
		ok = true;
	} else {
		LOG_WRN("read_battery: no battery report (%s)", camlink_result_str(r));
	}
	return ok;
}

const struct stored_image *get_image(void)
{
	return have_image ? &current : NULL;
}

void camera_node_forget_pairing(void)
{
	if (camlink_radio_on() == 0) {
		camlink_forget_all();
		camlink_radio_off();
	}
}
