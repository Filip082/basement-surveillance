/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BS_PROTOCOL_H
#define BS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define BS_PROTOCOL_VERSION 1U
#define BS_HEADER_SIZE 22U
#define BS_RADIO_MAX 255U
#define BS_PART_MAX (BS_RADIO_MAX - BS_HEADER_SIZE)
#define BS_PART_DEFAULT 200U
#define BS_NACK_MAX 32U
#define BS_CONTROL_MAX (BS_HEADER_SIZE + 1U + 2U * BS_NACK_MAX)
/* Match the actual Python MessageStore limits, not just the wire field widths. */
#define BS_MESSAGE_MAX 1000000U
#define BS_FRAGMENT_MAX 10000U

/**
 * @brief Packet kinds carried in the version 1 wire header.
 *
 * ENTRY, IMAGE and TELEMETRY carry DATA fragments. ACK, NACK and END
 * reference a whole message and carry its original DATA kind in their body.
 * These numeric values are part of the wire contract and must remain stable.
 */
enum bs_kind {
	BS_ENTRY = 1,     /**< DATA containing part of a UTF-8 entry-event JSON object. */
	BS_IMAGE = 2,     /**< DATA containing part of a raw JPEG image. */
	BS_TELEMETRY = 3, /**< DATA containing part of a UTF-8 telemetry JSON object. */
	BS_ACK = 4,       /**< Router confirms durable receipt of the complete message. */
	BS_NACK = 5,      /**< Router requests up to BS_NACK_MAX missing DATA indices. */
	BS_END = 6,       /**< Sender ends a DATA round and requests whole-message feedback. */
};

/**
 * @brief Stable message identity, checksum and fragmentation plan for a send session.
 *
 * Initialize with bs_message_init() and retain unchanged across retransmissions.
 * This native C layout can contain padding; serialize through the protocol encoders,
 * never by copying the struct directly into a radio packet.
 */
struct bs_message {
	uint32_t message_id; /**< Application message ID; all retries reuse this value. */
	uint32_t total_size; /**< Complete nonempty application body length in bytes. */
	uint32_t crc32;      /**< CRC32/ISO-HDLC of the complete body, matching zlib.crc32(). */
	uint16_t device_id;  /**< Provisioned identity of the sending node. */
	uint16_t count;      /**< Total distinct DATA fragments expected by the receiver. */
	uint16_t part_size;  /**< Local DATA payload limit, 1..BS_PART_MAX; not a wire field. */
	uint8_t kind;        /**< Original DATA category: BS_ENTRY, BS_IMAGE or BS_TELEMETRY. */
};

/**
 * @brief Validated whole-message ACK or bounded missing-fragment NACK.
 *
 * Output is valid only after bs_decode_feedback() returns zero. The decoder has
 * already checked that all message metadata matches the pending bs_message.
 */
struct bs_feedback {
	uint16_t missing[BS_NACK_MAX]; /**< Ascending, unique, zero-based missing DATA indices. */
	uint8_t missing_count;        /**< Valid array entries: zero for ACK, 1..BS_NACK_MAX for NACK. */
	uint8_t kind;                 /**< Feedback operation: BS_ACK or BS_NACK. */
};

/** CRC32/ISO-HDLC (reflected polynomial 0xedb88320), identical to zlib.crc32().
 * @param data Input bytes; may be NULL only when size is zero.
 * @param size Number of input bytes.
 * @return Complete body checksum; empty input yields zero.
 */
uint32_t bs_crc32(const uint8_t *data, size_t size);

/** Validate a nonempty application body and compute stable fragmentation metadata.
 * @param message Output metadata, valid only on success.
 * @param kind ENTRY, IMAGE or TELEMETRY.
 * @param device_id Provisioned sender identity.
 * @param message_id Unique ID reserved before sending, reused for all retries.
 * @param data Body that must remain unchanged for the complete session.
 * @param size Body length, at most BS_MESSAGE_MAX bytes.
 * @param part_size DATA payload length, 1..BS_PART_MAX (normally 200).
 * @return Zero on success, -EINVAL for invalid arguments, -EMSGSIZE for receiver limits.
 */
int bs_message_init(struct bs_message *message, uint8_t kind, uint16_t device_id,
		    uint32_t message_id, const uint8_t *data, size_t size, uint16_t part_size);

/** Encode one DATA frame, using explicit big-endian byte operations.
 * @param message Metadata initialized by bs_message_init().
 * @param data Unchanged full application body.
 * @param size Body length matching the metadata.
 * @param index Zero-based DATA fragment index.
 * @param frame Output packet buffer.
 * @param capacity Available output buffer bytes.
 * @return Encoded frame length or a negative errno value.
 */
int bs_encode_data(const struct bs_message *message, const uint8_t *data, size_t size,
		   uint16_t index, uint8_t *frame, size_t capacity);

/** Encode an END referencing the full message.
 * @param message Validated pending-message metadata.
 * @param frame Output packet buffer.
 * @param capacity Available output bytes (at least 23).
 * @return Encoded length or a negative errno value.
 */
int bs_encode_end(const struct bs_message *message, uint8_t *frame, size_t capacity);

/** Decode ACK/NACK and require every identity/size/checksum field to match.
 * @param message Pending-message metadata.
 * @param frame Received binary radio payload.
 * @param size Received byte count.
 * @param feedback Output ACK or sorted, bounded NACK indices, valid only on success.
 * @return Zero on success; -ENOMSG for another message, -EBADMSG for malformed frames,
 *         or -EINVAL for invalid caller arguments.
 */
int bs_decode_feedback(const struct bs_message *message, const uint8_t *frame, size_t size,
		       struct bs_feedback *feedback);
#endif
