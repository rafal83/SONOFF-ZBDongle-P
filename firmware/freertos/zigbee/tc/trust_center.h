/* SPDX-License-Identifier: MIT
 * Centralized Trust Center frame builders (Zigbee 3.0 / R21+).
 * Only frame construction lives here; policy (who may join, link-key
 * storage, counters persistence) belongs to the future TC state machine.
 */
#ifndef ZB_TRUST_CENTER_H
#define ZB_TRUST_CENTER_H
#include <stddef.h>
#include <stdint.h>

#define APS_CMD_TRANSPORT_KEY 0x05u
#define APS_KEY_TYPE_STANDARD_NWK 0x01u
/* Global default Trust Center link key "ZigBeeAlliance09". */
extern const uint8_t TC_DEFAULT_LINK_KEY[16];

typedef struct {
    const uint8_t *tc_link_key;      /* TCLK of the joining device */
    const uint8_t *network_key;
    uint8_t key_seq;
    uint16_t dst_short;
    uint64_t dst_ext, tc_ext;
    uint8_t nwk_seq, radius, aps_counter;
    uint32_t aps_frame_counter;      /* TC outgoing APS counter for that link key */
} tc_transport_key_t;

/* Complete NWK frame (NWK-unsecured, APS-secured with the key-transport key)
 * delivering the network key to a device that just associated. */
size_t tc_build_transport_network_key(const tc_transport_key_t *p, uint8_t *out, size_t cap);
#endif
