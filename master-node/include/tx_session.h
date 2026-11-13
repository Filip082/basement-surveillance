/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BS_TX_SESSION_H
#define BS_TX_SESSION_H

#include "protocol.h"
#include <stdint.h>

enum bs_tx_event {
	BS_TX_END, BS_TX_ACK, BS_TX_NACK, BS_TX_MISSING,
	BS_TX_TIMEOUT, BS_TX_IO_ERROR, BS_TX_IGNORED,
};

/** Blocking transport contract. send() includes duty scheduling and charges failed attempts.
 * recv() returns a packet length, -ETIMEDOUT/-EAGAIN, or another negative errno.
 * now_ms() is monotonic. event() is optional and must not access the radio.
 */
struct bs_tx_io {
	void *context;
	int (*send)(void *context, const uint8_t *frame, size_t size);
	int (*recv)(void *context, uint8_t *frame, size_t capacity, uint32_t timeout_ms);
	uint64_t (*now_ms)(void *context);
	void (*event)(void *context, enum bs_tx_event event, int detail);
};

struct bs_tx_options {
	uint32_t feedback_timeout_ms;
	uint16_t part_size;
	uint16_t max_end_attempts;
	uint8_t max_packet_attempts;
};

struct bs_tx_result {
	uint32_t data_attempts;
	uint32_t end_attempts;
	uint32_t ignored_feedback;
	uint16_t fragment_count;
};

/** Compute conservative start-to-start spacing, rounded up in milliseconds.
 * @param airtime_ms Planned TX duration including timing margin.
 * @param duty_permille Allowed fraction in 1..1000; e.g. 10 means 1%.
 * @return Minimum spacing, or zero for invalid duty/zero airtime.
 * The transport charges spacing minus airtime AFTER completion, even on TX failure.
 */
uint64_t bs_tx_spacing_ms(uint32_t airtime_ms, uint16_t duty_permille);

/** Send one whole message, retaining its IDs and checksum across every retransmission.
 * @param io Single-owner blocking radio adapter, or a host fake implementing the contract.
 * @param options Bounded retry counts, fragmentation and a duty-aware feedback deadline.
 * @param kind ENTRY, IMAGE or TELEMETRY.
 * @param device_id Provisioned node identity.
 * @param message_id Persistently reserved ID; not changed by this function.
 * @param data Unchanged body, kept alive until this blocking call returns.
 * @param size Nonzero application byte count.
 * @param result Output counters, including unsuccessful TX attempts.
 * @return Zero ONLY after a matching ACK; -ETIMEDOUT on exhausted END rounds,
 *         transport errno on unrecovered I/O failure, or protocol argument errors.
 * This API owns no queue or thread. A future queue can invoke it from its own worker.
 */
int bs_tx_send(const struct bs_tx_io *io, const struct bs_tx_options *options,
	       uint8_t kind, uint16_t device_id, uint32_t message_id,
	       const uint8_t *data, size_t size, struct bs_tx_result *result);
#endif
