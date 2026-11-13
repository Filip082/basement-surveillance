/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BS_MESSAGE_IDS_H
#define BS_MESSAGE_IDS_H
#include <stdint.h>

/** Mount the board's existing storage partition for persistent ID reservations.
 * @return Zero on success, negative errno on corrupt/unavailable storage. Never silently resets IDs.
 */
int bs_message_ids_init(void);

/** Reserve a never-before-issued message ID, committing blocks before exposing IDs.
 * @param message_id Output ID. Serialize callers; the demo calls this only from main.
 * @return Zero on success, negative errno on flash failure or sequence exhaustion.
 * A reset skips unused reserved IDs. Erasing/replacing flash requires reprovisioning the device ID.
 */
int bs_message_id_next(uint32_t *message_id);
#endif
