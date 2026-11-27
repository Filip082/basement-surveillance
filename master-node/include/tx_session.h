/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BS_TX_SESSION_H
#define BS_TX_SESSION_H

#include "protocol.h"
#include <stdint.h>

/**
 * @brief Sender progress notifications delivered through bs_tx_io.event().
 *
 * Each event defines the meaning of the callback's integer detail argument.
 * These are local diagnostics, not packet kinds or values serialized over LoRa.
 */
enum bs_tx_event {
	BS_TX_END,     /**< Starting an END/feedback round; detail is its one-based number. */
	BS_TX_ACK,     /**< Matching ACK received; detail is the one-based round number. */
	BS_TX_NACK,    /**< Matching NACK received; detail is the number of missing indices. */
	BS_TX_MISSING, /**< Retransmitting DATA; detail is its zero-based fragment index. */
	BS_TX_TIMEOUT, /**< No matching feedback before the deadline; detail is the round number. */
	BS_TX_IO_ERROR, /**< Packet send failed; detail is the transport's negative errno. */
	BS_TX_IGNORED, /**< Malformed/unrelated feedback ignored; detail is received byte count. */
};

/**
 * @brief Blocking, single-owner transport callbacks used by the portable sender.
 *
 * The application or host test supplies these operations. Every callback receives
 * context; send(), recv() and now_ms() are required, while event() is optional.
 * Buffers are borrowed for the duration of each call and must not be retained.
 */
struct bs_tx_io {
	void *context; /**< Caller-owned callback state; NULL is allowed for stateless adapters. */

	/**
	 * @brief Schedule and attempt one binary packet transmission.
	 * @param context Caller state from this callback table.
	 * @param frame Immutable packet bytes, including the protocol header.
	 * @param size Packet length in bytes, at most BS_RADIO_MAX.
	 * @return Zero on TX completion, negative errno on failure; not a delivery ACK.
	 * Include duty-cycle waiting and charge failed TX attempts before allowing another TX.
	 */
	int (*send)(void *context, const uint8_t *frame, size_t size);

	/**
	 * @brief Wait for one received packet within the remaining feedback window.
	 * @param context Caller state from this callback table.
	 * @param frame Output buffer for the complete received packet.
	 * @param capacity Available buffer length in bytes.
	 * @param timeout_ms Maximum wait in milliseconds; malformed traffic must not reset it.
	 * @return Received length, -ETIMEDOUT/-EAGAIN on timeout, or another negative errno.
	 */
	int (*recv)(void *context, uint8_t *frame, size_t capacity, uint32_t timeout_ms);

	/**
	 * @brief Read the monotonic clock used to bound a feedback round.
	 * @param context Caller state from this callback table.
	 * @return Nondecreasing elapsed time in milliseconds, unaffected by wall-clock changes.
	 */
	uint64_t (*now_ms)(void *context);

	/**
	 * @brief Optionally report session progress without taking ownership of the radio.
	 * @param context Caller state from this callback table.
	 * @param event Progress notification defining how to interpret detail.
	 * @param detail Round, fragment index, count or error as documented by bs_tx_event.
	 * @return Nothing. Keep handling short and do not reenter radio operations.
	 */
	void (*event)(void *context, enum bs_tx_event event, int detail);
};

/**
 * @brief Fragmentation, receive timing and finite retry limits for one message.
 *
 * Set every field before bs_tx_send(). The receive window must cover the router's
 * quiet period and duty-limited feedback; a packet TX retry is distinct from an
 * END/feedback round and does not create a new application message.
 */
struct bs_tx_options {
	uint32_t feedback_timeout_ms; /**< Nonzero RX window after each sent END, in milliseconds. */
	uint16_t part_size;           /**< DATA payload limit, 1..BS_PART_MAX; normally 200 bytes. */
	uint16_t max_end_attempts;    /**< Maximum END/feedback rounds, including the first; at least one. */
	uint8_t max_packet_attempts;  /**< Send calls per DATA/END packet, including the first; at least one. */
};

/**
 * @brief Delivery-attempt counters produced by a sender session.
 *
 * Attempts include failed TX calls and retransmissions. These statistics do not
 * prove delivery: only a zero return from bs_tx_send() confirms a matching ACK.
 */
struct bs_tx_result {
	uint32_t data_attempts;    /**< Total DATA send calls across initial sends and retries. */
	uint32_t end_attempts;     /**< Total END send calls, including retries within a round. */
	uint32_t ignored_feedback; /**< Received frames rejected as malformed or unrelated. */
	uint16_t fragment_count;   /**< Distinct DATA fragments in the original message, excluding retries. */
};

/** Compute conservative start-to-start spacing, rounded up in milliseconds.
 * @param airtime_ms Planned TX duration including timing margin.
 * @param duty_permille Allowed fraction in 1..1000; 10 means 1%, and 0 disables duty spacing.
 * @return Minimum spacing; disabled duty returns airtime_ms (no off-time).
 *         Returns zero for duty above 1000 or zero airtime.
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
