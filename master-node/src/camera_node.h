/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public interface to the camera node. This is everything the rest of the firmware
 * needs:
 *
 *     camera_node_init();                 // once at start-up
 *     if (take_image()) {                 // wake the camera, receive its picture, BLE off
 *         const struct stored_image *img = get_image();   // picture in RAM
 *         ... use img->data / img->len (e.g. send over LoRa) ...
 *     }
 *
 * Bluetooth is only on while take_image() runs.
 */
#pragma once

#include <stdbool.h>

#include "image_store.h"

/* Set up the wake line and the Bluetooth module. Bluetooth itself stays off.
 * Returns 0 or a negative errno. */
int camera_node_init(void);

/*
 * Wake the camera node, receive one picture over Bluetooth LE and turn Bluetooth
 * off again. Blocks for the whole time (about 2-15 s). Returns true if a complete,
 * CRC-checked picture is now available through get_image().
 *
 * Pairing: if no camera is paired yet, it pairs with the first camera that answers
 * (first use) and stores the pairing in flash; otherwise it only talks to the
 * paired camera. The previous picture is overwritten by a new attempt, so after a
 * false return get_image() has nothing.
 */
bool take_image(void);

/* The last picture received by take_image(), or NULL if there is none. The data
 * stays valid in RAM until the next take_image() call. Do not call while
 * take_image() is running. */
const struct stored_image *get_image(void);

struct battery_info {
	uint8_t percent;        /* 0..100 */
	uint16_t battery_mv;    /* battery voltage, calculated by the camera node */
	uint16_t adc_mv;        /* voltage at the camera node's ADC pin */
};

/*
 * Ask the camera node for its battery level: a long wake pulse (APP_BATTERY_WAKE_PULSE_MS)
 * tells it to measure and send the battery report instead of a picture. Bluetooth is on
 * only while this runs. Returns true and fills *out when the report arrived; it is also
 * logged ("BATTERY: 73 % ..."). Blocks for about 1-5 s.
 */
bool read_battery(struct battery_info *out);

/* Forget the paired camera (the camera node keeps its own record; reset it too). */
void camera_node_forget_pairing(void);
