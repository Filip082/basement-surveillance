/* SPDX-License-Identifier: Apache-2.0 */
#include "protocol.h"

#include <errno.h>
#include <stdbool.h>

static uint16_t get16(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static bool data_kind(uint8_t kind)
{
	return kind >= BS_ENTRY && kind <= BS_TELEMETRY;
}

static bool valid_message(const struct bs_message *m)
{
	return m != NULL && data_kind(m->kind) && m->total_size > 0 &&
	       m->total_size <= BS_MESSAGE_MAX && m->part_size > 0 &&
	       m->part_size <= BS_PART_MAX && m->count > 0 && m->count <= BS_FRAGMENT_MAX &&
	       m->count == (m->total_size + m->part_size - 1U) / m->part_size;
}

uint32_t bs_crc32(const uint8_t *data, size_t size)
{
	uint32_t crc = UINT32_MAX;

	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
		}
	}
	return ~crc;
}

int bs_message_init(struct bs_message *m, uint8_t kind, uint16_t device_id,
		    uint32_t message_id, const uint8_t *data, size_t size, uint16_t part_size)
{
	if (m == NULL || data == NULL || size == 0 || !data_kind(kind) ||
	    part_size == 0 || part_size > BS_PART_MAX) {
		return -EINVAL;
	}
	if (size > BS_MESSAGE_MAX) {
		return -EMSGSIZE;
	}
	size_t count = (size + part_size - 1U) / part_size;

	if (count > BS_FRAGMENT_MAX) {
		return -EMSGSIZE;
	}
	*m = (struct bs_message) {
		.message_id = message_id, .total_size = (uint32_t)size,
		.crc32 = bs_crc32(data, size), .device_id = device_id,
		.count = (uint16_t)count, .part_size = part_size, .kind = kind,
	};
	return 0;
}

static void encode_header(const struct bs_message *m, uint8_t kind,
			  uint16_t index, uint8_t *frame)
{
	frame[0] = 'B';
	frame[1] = 'S';
	frame[2] = BS_PROTOCOL_VERSION;
	frame[3] = kind;
	put16(frame + 4, m->device_id);
	put32(frame + 6, m->message_id);
	put16(frame + 10, index);
	put16(frame + 12, m->count);
	put32(frame + 14, m->total_size);
	put32(frame + 18, m->crc32);
}

int bs_encode_data(const struct bs_message *m, const uint8_t *data, size_t size,
		   uint16_t index, uint8_t *frame, size_t capacity)
{
	if (!valid_message(m) || data == NULL || frame == NULL ||
	    size != m->total_size || index >= m->count) {
		return -EINVAL;
	}
	size_t offset = (size_t)index * m->part_size;
	size_t bytes = size - offset;

	if (bytes > m->part_size) {
		bytes = m->part_size;
	}
	if (capacity < BS_HEADER_SIZE + bytes) {
		return -ENOBUFS;
	}
	encode_header(m, m->kind, index, frame);
	for (size_t i = 0; i < bytes; ++i) {
		frame[BS_HEADER_SIZE + i] = data[offset + i];
	}
	return (int)(BS_HEADER_SIZE + bytes);
}

int bs_encode_end(const struct bs_message *m, uint8_t *frame, size_t capacity)
{
	if (!valid_message(m) || frame == NULL) {
		return -EINVAL;
	}
	if (capacity < BS_HEADER_SIZE + 1U) {
		return -ENOBUFS;
	}
	encode_header(m, BS_END, 0, frame);
	frame[BS_HEADER_SIZE] = m->kind;
	return BS_HEADER_SIZE + 1U;
}

int bs_decode_feedback(const struct bs_message *m, const uint8_t *frame, size_t size,
		       struct bs_feedback *feedback)
{
	if (!valid_message(m) || frame == NULL || feedback == NULL) {
		return -EINVAL;
	}
	if (size < BS_HEADER_SIZE + 1U || size > BS_CONTROL_MAX ||
	    frame[0] != 'B' || frame[1] != 'S' || frame[2] != BS_PROTOCOL_VERSION ||
	    (frame[3] != BS_ACK && frame[3] != BS_NACK) || get16(frame + 10) != 0 ||
	    !data_kind(frame[BS_HEADER_SIZE])) {
		return -EBADMSG;
	}
	uint16_t count = get16(frame + 12);
	uint32_t total = get32(frame + 14);
	size_t missing = (size - BS_HEADER_SIZE - 1U) / 2U;

	if (count == 0 || total < count || total > (uint32_t)count * BS_PART_MAX ||
	    (frame[3] == BS_ACK && size != BS_HEADER_SIZE + 1U) ||
	    (frame[3] == BS_NACK && (missing == 0 || missing > BS_NACK_MAX ||
				  (size - BS_HEADER_SIZE - 1U) % 2U != 0))) {
		return -EBADMSG;
	}
	struct bs_feedback result = {.kind = frame[3], .missing_count = (uint8_t)missing};

	for (size_t i = 0; i < missing; ++i) {
		uint16_t index = get16(frame + BS_HEADER_SIZE + 1U + 2U * i);

		if (index >= count || (i > 0 && index <= result.missing[i - 1U])) {
			return -EBADMSG;
		}
		result.missing[i] = index;
	}
	if (get16(frame + 4) != m->device_id || get32(frame + 6) != m->message_id ||
	    count != m->count || total != m->total_size ||
	    get32(frame + 18) != m->crc32 || frame[BS_HEADER_SIZE] != m->kind) {
		return -ENOMSG;
	}
	*feedback = result;
	return 0;
}
