/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BS_LORA_TRANSPORT_H
#define BS_LORA_TRANSPORT_H

#include "tx_session.h"

/** Initialize the lora0 SX1262 using Kconfig modulation and the existing board overlay.
 * @return Zero on success; negative errno for absent hardware, unsupported settings or readback failure.
 * Call once from thread context before sending. This component owns the radio exclusively.
 */
int bs_lora_init(void);

/** Deliver an arbitrary binary body using bounded DATA/END/ACK/NACK exchange.
 * @param kind ENTRY, IMAGE or TELEMETRY.
 * @param device_id Provisioned sender identity, unique among nodes.
 * @param message_id Unique application ID, retained across all internal retransmissions.
 * @param data Immutable body kept alive until this blocking call returns.
 * @param size Nonzero body length, up to BS_MESSAGE_MAX.
 * @param result Output delivery statistics; delivery succeeds only after a matching ACK.
 * @return Zero after durable router receipt; negative errno on timeout/error, -EBUSY if another
 *         application thread is already using this radio. Call from thread context, never an ISR.
 */
int bs_lora_send_message(uint8_t kind, uint16_t device_id, uint32_t message_id,
			 const uint8_t *data, size_t size, struct bs_tx_result *result);
#endif
