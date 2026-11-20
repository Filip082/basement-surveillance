/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RAM store for the last picture received from the camera node.
 *
 * One static buffer of CONFIG_APP_IMAGE_BUF_SIZE bytes. A picture is written in
 * three steps (begin -> append... -> commit); it only becomes visible through
 * image_store_get() after commit, so a half-received or corrupt picture is never
 * handed out. Starting a new picture discards the previous one (single buffer).
 *
 * The data stays valid until the next begin(). It is lost on reset / System OFF
 * (RAM). Not thread safe: the BLE thread writes, the reader must only call
 * image_store_get() while no transfer is running (see camlink_receive_image()).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct stored_image {
	const uint8_t *data;   /* JPEG bytes, points into the static buffer */
	uint32_t len;          /* number of valid bytes */
	uint16_t width;        /* as announced by the camera */
	uint16_t height;
	bool face_detected;    /* note from the camera node: a face was found (and framed) */
	uint8_t face_count;    /* number of faces it found */
	int64_t saved_at_ms;   /* k_uptime_get() when the picture was committed */
};

/* Start a new picture of `len` bytes. Discards the stored one.
 * Returns 0, -EINVAL (len == 0) or -ENOSPC (does not fit). */
int image_store_begin(uint32_t len, uint16_t width, uint16_t height, bool face_detected,
		      uint8_t face_count);

/* Append received bytes. Returns 0 or -EOVERFLOW (more than announced / than fits). */
int image_store_append(const uint8_t *data, uint32_t n);

/* Bytes stored so far / bytes announced for the current picture. */
uint32_t image_store_received(void);
uint32_t image_store_expected(void);

/* CRC32 (zlib/IEEE, same as the camera sends) of the bytes stored so far. */
uint32_t image_store_crc(void);

/* Mark the current picture complete and valid. */
void image_store_commit(void);

/* Throw the current (incomplete / corrupt) picture away. */
void image_store_abort(void);

/* Fetch the last committed picture. Returns false if there is none. */
bool image_store_get(struct stored_image *out);
