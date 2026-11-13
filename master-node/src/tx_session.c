/* SPDX-License-Identifier: Apache-2.0 */
#include "tx_session.h"

#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

uint64_t bs_tx_spacing_ms(uint32_t airtime_ms, uint16_t duty_permille)
{
	if (airtime_ms == 0 || duty_permille == 0 || duty_permille > 1000) {
		return 0;
	}
	return ((uint64_t)airtime_ms * 1000U + duty_permille - 1U) / duty_permille;
}

static void event(const struct bs_tx_io *io, enum bs_tx_event code, int detail)
{
	if (io->event != NULL) {
		io->event(io->context, code, detail);
	}
}

static int send_packet(const struct bs_tx_io *io, const struct bs_tx_options *options,
		       const uint8_t *frame, size_t size, uint32_t *attempts)
{
	int ret = -EIO;

	for (unsigned i = 0; i < options->max_packet_attempts; ++i) {
		++*attempts;
		ret = io->send(io->context, frame, size);
		if (ret == 0) {
			return 0;
		}
		event(io, BS_TX_IO_ERROR, ret);
	}
	return ret;
}

int bs_tx_send(const struct bs_tx_io *io, const struct bs_tx_options *options,
	       uint8_t kind, uint16_t device_id, uint32_t message_id,
	       const uint8_t *data, size_t size, struct bs_tx_result *result)
{
	if (io == NULL || options == NULL || result == NULL || io->send == NULL ||
	    io->recv == NULL || io->now_ms == NULL || options->max_end_attempts == 0 ||
	    options->max_packet_attempts == 0 || options->feedback_timeout_ms == 0) {
		return -EINVAL;
	}
	memset(result, 0, sizeof(*result));
	struct bs_message message;
	struct bs_feedback feedback;
	uint8_t packet[BS_RADIO_MAX];
	int ret = bs_message_init(&message, kind, device_id, message_id,
				 data, size, options->part_size);

	if (ret < 0) {
		return ret;
	}
	result->fragment_count = message.count;
	/* Initial DATA burst. The transport paces every packet, including retries. */
	for (uint16_t i = 0; i < message.count; ++i) {
		ret = bs_encode_data(&message, data, size, i, packet, sizeof(packet));
		if (ret < 0) {
			return ret;
		}
		ret = send_packet(io, options, packet, (size_t)ret, &result->data_attempts);
		if (ret < 0) {
			return ret;
		}
	}
	for (unsigned round = 0; round < options->max_end_attempts; ++round) {
		ret = bs_encode_end(&message, packet, sizeof(packet));
		if (ret < 0) {
			return ret;
		}
		event(io, BS_TX_END, (int)(round + 1U));
		ret = send_packet(io, options, packet, (size_t)ret, &result->end_attempts);
		if (ret < 0) {
			return ret;
		}
		uint64_t deadline = io->now_ms(io->context) + options->feedback_timeout_ms;
		bool have_nack = false;

		while (true) {
			uint64_t now = io->now_ms(io->context);

			if (now >= deadline) {
				break;
			}
			uint64_t remaining = deadline - now;
			ret = io->recv(io->context, packet, sizeof(packet), (uint32_t)remaining);
			if (ret == -ETIMEDOUT || ret == -EAGAIN) {
				break;
			}
			if (ret < 0) {
				return ret;
			}
			if (bs_decode_feedback(&message, packet, (size_t)ret, &feedback) < 0) {
				++result->ignored_feedback;
				event(io, BS_TX_IGNORED, ret);
				continue;
			}
			if (feedback.kind == BS_ACK) {
				event(io, BS_TX_ACK, (int)(round + 1U));
				return 0;
			}
			event(io, BS_TX_NACK, feedback.missing_count);
			have_nack = true;
			break;
		}
		if (!have_nack) {
			event(io, BS_TX_TIMEOUT, (int)(round + 1U));
			/* Lost feedback retries ONLY END, never the DATA burst. */
			continue;
		}
		if (round + 1U == options->max_end_attempts) {
			break;
		}
		for (unsigned i = 0; i < feedback.missing_count; ++i) {
			event(io, BS_TX_MISSING, feedback.missing[i]);
			ret = bs_encode_data(&message, data, size, feedback.missing[i],
					     packet, sizeof(packet));
			if (ret < 0) {
				return ret;
			}
			ret = send_packet(io, options, packet, (size_t)ret, &result->data_attempts);
			if (ret < 0) {
				return ret;
			}
		}
	}
	return -ETIMEDOUT;
}
