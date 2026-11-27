/* SPDX-License-Identifier: Apache-2.0 */

#include "image_store.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>

static uint8_t buf[CONFIG_APP_IMAGE_BUF_SIZE] __aligned(4);

static struct {
	uint32_t expected;
	uint32_t received;
	uint32_t crc;
	uint16_t width;
	uint16_t height;
	enum image_status status;
	uint8_t human_count;
	uint8_t face_count;
	bool valid;           /* a committed picture is in buf */
	int64_t saved_at_ms;
} st;

int image_store_begin(uint32_t len, uint16_t width, uint16_t height, enum image_status status,
		      uint8_t human_count, uint8_t face_count)
{
	st.valid = false;
	st.received = 0;
	st.crc = 0;
	st.expected = 0;

	if (len == 0) {
		return -EINVAL;
	}
	if (len > sizeof(buf)) {
		return -ENOSPC;
	}
	st.expected = len;
	st.width = width;
	st.height = height;
	st.status = status;
	st.human_count = human_count;
	st.face_count = face_count;
	return 0;
}

int image_store_append(const uint8_t *data, uint32_t n)
{
	if (st.received + n > st.expected) {
		return -EOVERFLOW;
	}
	memcpy(&buf[st.received], data, n);
	st.crc = crc32_ieee_update(st.crc, data, n);
	st.received += n;
	return 0;
}

uint32_t image_store_received(void)
{
	return st.received;
}

uint32_t image_store_expected(void)
{
	return st.expected;
}

uint32_t image_store_crc(void)
{
	return st.crc;
}

void image_store_commit(void)
{
	st.valid = true;
	st.saved_at_ms = k_uptime_get();
}

void image_store_abort(void)
{
	st.valid = false;
	st.received = 0;
	st.expected = 0;
	st.crc = 0;
}

bool image_store_get(struct stored_image *out)
{
	if (!st.valid) {
		return false;
	}
	out->data = buf;
	out->len = st.received;
	out->width = st.width;
	out->height = st.height;
	out->status = st.status;
	out->human_count = st.human_count;
	out->face_count = st.face_count;
	out->saved_at_ms = st.saved_at_ms;
	return true;
}

const char *image_status_str(enum image_status status)
{
	switch (status) {
	case IMAGE_NO_HUMAN: return "no human detected";
	case IMAGE_HUMAN:    return "human detected";
	case IMAGE_FACE:     return "face detected";
	}
	return "?";
}
