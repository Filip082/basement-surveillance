#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * BLE link used to push one JPEG to a paired computer.
 * The NimBLE stack is started when needed and completely shut down afterwards
 * (ble_link_stop) so that nothing in the Bluetooth controller draws power
 * while the node sleeps. Pairing keys are stored in NVS and survive that.
 *
 * GATT service dfa10001-5b3c-4f6d-9e21-0a4c7b8d1e00
 *   Control dfa10002-...  write   1 byte: 0x01 = frame received OK (ACK), 0x02 = resend (NAK)
 *   Image   dfa10003-...  notify  START / DATA... / END packets (see README)
 */

typedef enum {
    BLE_ACK_OK,
    BLE_ACK_NAK,
    BLE_ACK_TIMEOUT,
} ble_ack_t;

/* Delete all stored pairings. Only call while the stack is NOT running. */
void ble_link_erase_bonds(void);

/* Start the stack. advertise=true also starts connectable advertising as soon as the
 * stack is ready; false only brings the stack up (used for the boot self-test). */
esp_err_t ble_link_start(bool advertise);

/* Wait until the stack is synced with the controller (address known). */
esp_err_t ble_link_wait_synced(uint32_t timeout_ms);

/* Number of paired devices stored (valid while the stack is running). */
int ble_link_bond_count(void);

/* Wait until a computer is connected, the link is encrypted (paired) and it has
 * subscribed to the Image characteristic. */
esp_err_t ble_link_wait_ready(uint32_t timeout_ms);

/* Push one JPEG as notifications (START, DATA..., END with CRC32). Needs wait_ready() first. */
esp_err_t ble_link_send_image(const uint8_t *jpeg, size_t len, uint16_t width, uint16_t height);

/* Wait for the computer to confirm (or reject) the frame via the Control characteristic. */
ble_ack_t ble_link_wait_ack(uint32_t timeout_ms);

/* Disconnect, stop advertising and shut the whole stack down. Safe to call at any time. */
void ble_link_stop(void);
