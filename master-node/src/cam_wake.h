/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The line that wakes the camera node.
 *
 * Pin and polarity come from the devicetree overlay of the board:
 *   /zephyr,user { cam-wake-gpios = <&gpioX N GPIO_ACTIVE_LOW>; };
 * "Active" = the camera node must wake up. Idle = the pin is driven to the inactive
 * level (HIGH for an active-LOW line), push-pull.
 */
#pragma once

/* Configure the pin as an output in its inactive (idle) state. Returns 0 or -errno. */
int cam_wake_init(void);

/* Drive the line active for CONFIG_APP_CAM_WAKE_PULSE_MS, then idle again. Blocks. */
void cam_wake_pulse(void);
