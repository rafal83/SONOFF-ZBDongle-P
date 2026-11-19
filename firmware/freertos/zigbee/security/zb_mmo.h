/* SPDX-License-Identifier: MIT
 * Zigbee cryptographic hash (Matyas-Meyer-Oseas over AES-128, Zigbee spec
 * B.6), keyed hash HMAC-MMO (B.1.4) and install-code helpers.
 */
#ifndef ZB_MMO_H
#define ZB_MMO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Messages shorter than 2^16 bits (8191 bytes); returns false otherwise. */
bool zb_mmo_hash(const uint8_t *msg, size_t len, uint8_t digest[16]);
bool zb_hmac_mmo(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]);
/* Zigbee key derivations from a link key (input byte 0x00 / 0x02). */
void zb_key_transport_key(const uint8_t link_key[16], uint8_t out[16]);
void zb_key_load_key(const uint8_t link_key[16], uint8_t out[16]);
/* CRC-16/X-25 as appended (little endian) to 6/8/12/16-byte install codes. */
uint16_t zb_install_code_crc(const uint8_t *code, size_t len);
/* Validates the CRC and derives the preconfigured link key. len includes
 * the 2 CRC bytes (8, 10, 14 or 18). */
bool zb_install_code_to_key(const uint8_t *code_with_crc, size_t len, uint8_t key[16]);
#endif
