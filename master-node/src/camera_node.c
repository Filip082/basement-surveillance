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

	cam_wake_pulse();                                               /* wake the camera */
	camlink_result_t r = camlink_receive_image(!paired, CONFIG_APP_SESSION_TIMEOUT_S * 1000);

	if (r == CAMLINK_BOND_LOST && IS_ENABLED(CONFIG_APP_AUTO_REPAIR)) {
		LOG_WRN("camera forgot us: waking it again and pairing from scratch");
		cam_wake_pulse();
		r = camlink_receive_image(true, CONFIG_APP_SESSION_TIMEOUT_S * 1000);
	}

	camlink_radio_off();                                            /* Bluetooth off */

	if (r == CAMLINK_OK && image_store_get(&current)) {
		have_image = true;
		LOG_INF("take_image: picture ready, %u bytes (%ux%u), %s", current.len, current.width,
			current.height,
			current.face_detected ? "face detected" : "no face detected");
	} else {
		LOG_WRN("take_image: no picture (%s)", camlink_result_str(r));
	}
	return have_image;
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
