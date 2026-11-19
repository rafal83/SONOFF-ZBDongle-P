/* SPDX-License-Identifier: MIT
 * APS layer security (Zigbee spec 4.4): ENC-MIC-32 over an APS frame with
 * the auxiliary header of 4.5.1 (same layout as the NWK one, reused from
 * nwk_frame). The caller supplies the CCM key actually used: the link key
 * for key id 0, or the HMAC-MMO derived key-transport (2) / key-load (3) key.
 */
#ifndef ZB_APS_SECURITY_H
#define ZB_APS_SECURITY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nwk_frame.h"

enum { APS_KEY_DATA = 0, APS_KEY_NETWORK = 1, APS_KEY_TRANSPORT = 2, APS_KEY_LOAD = 3 };
#define APS_SECURITY_LEVEL 5u   /* ENC-MIC-32 */

/* Seal: out = APS header (security bit forced) | aux header (on-air level 0)
 * | ciphertext | MIC. Returns the total length, 0 on error. */
size_t aps_seal(const uint8_t key[16], const uint8_t *aps_header, size_t header_len,
                const nwk_aux_t *aux, const uint8_t *payload, size_t payload_len,
                uint8_t *out, size_t cap);
/* Open in place. On success *aux is filled and the plaintext starts at
 * frame + *payload_offset. On failure the payload bytes are wiped. */
bool aps_open(const uint8_t key[16], uint8_t *frame, size_t len, size_t header_len,
              nwk_aux_t *aux, size_t *payload_offset, size_t *payload_len);
#endif
