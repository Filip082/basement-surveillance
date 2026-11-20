/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth LE link to the camera node (we are the central / GATT client).
 *
 * One call, camlink_receive_image(), runs a complete session:
 *   scan -> connect -> pair/encrypt -> discover service -> subscribe ->
 *   receive picture into image_store -> ACK -> disconnect.
 * The protocol is the one of the camera firmware (see README "Protocol").
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	CAMLINK_OK,          /* picture received, CRC ok, stored in image_store, ACK sent */
	CAMLINK_NO_CAMERA,   /* nothing advertising before the timeout */
	CAMLINK_TIMEOUT,     /* found the camera but did not finish in time */
	CAMLINK_FAILED,      /* link lost / security or discovery failed / bad data */
	CAMLINK_BOND_LOST,   /* camera does not know us any more; bond deleted (see APP_AUTO_REPAIR) */
	CAMLINK_TOO_BIG,     /* picture larger than CONFIG_APP_IMAGE_BUF_SIZE */
	CAMLINK_NO_BOND,     /* normal mode requested but no camera is paired */
	CAMLINK_BUSY,        /* a session is already running */
} camlink_result_t;

/* Prepare the module (semaphores, work items). Call once at start-up. Bluetooth is
 * NOT started here, so the radio stays off until camlink_radio_on(). */
void camlink_init(void);

/* Switch Bluetooth on (bt_enable + load the bonds from flash). 0 or -errno. */
int camlink_radio_on(void);

/* Switch Bluetooth off again (bt_disable). Bonds stay in flash. */
void camlink_radio_off(void);

/* The following three need the radio to be on (bonds are loaded then). */

/* Number of paired cameras stored in flash. */
int camlink_bond_count(void);

/* Delete all stored bonds (this side only: the camera keeps its own copy). */
void camlink_forget_all(void);

/*
 * Run one session. Blocks until it is over.
 *
 * pairing   true  = accept ANY device advertising CONFIG_APP_CAMERA_NAME and pair
 *                   with it (first use);
 *           false = only talk to the already paired camera (address must match
 *                   the stored bond) - returns CAMLINK_NO_BOND if there is none.
 * find_timeout_ms  how long to wait for the camera to appear. Once it is found,
 *                   CONFIG_APP_TRANSFER_TIMEOUT_S applies to the rest.
 *
 * Progress and the number of saved bytes are logged. On CAMLINK_OK the picture
 * can be read with image_store_get().
 */
camlink_result_t camlink_receive_image(bool pairing, uint32_t find_timeout_ms);

/* Readable name of a result, for logs. */
const char *camlink_result_str(camlink_result_t r);
