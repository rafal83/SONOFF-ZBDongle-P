/* SPDX-License-Identifier: MIT
 * Transport Key command layout: Zigbee spec 4.4.10.1 (standard network key:
 * key type, key, sequence number, destination address, source address).
 */
#include "trust_center.h"
#include "aps_security.h"
#include "nwk_frame.h"
#include "zb_mmo.h"

const uint8_t TC_DEFAULT_LINK_KEY[16] = {'Z', 'i', 'g', 'B', 'e', 'e', 'A', 'l',
                                         'l', 'i', 'a', 'n', 'c', 'e', '0', '9'};

static void wr64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8u * i)); }

size_t tc_build_transport_network_key(const tc_transport_key_t *p, uint8_t *out, size_t cap) {
    if (!p || !p->tc_link_key || !p->network_key || !out || p->dst_short >= 0xFFF8u) return 0;
    nwk_header_t nwk = {0};
    nwk.type = NWK_TYPE_DATA;
    nwk.dst = p->dst_short;
    nwk.src = 0x0000;
    nwk.radius = p->radius;
    nwk.seq = p->nwk_seq;
    size_t n = nwk_build(&nwk, out, cap);
    if (!n) return 0;
    uint8_t payload[2 + 16 + 1 + 8 + 8];
    payload[0] = APS_CMD_TRANSPORT_KEY;
    payload[1] = APS_KEY_TYPE_STANDARD_NWK;
    for (unsigned i = 0; i < 16; i++) payload[2 + i] = p->network_key[i];
    payload[18] = p->key_seq;
    wr64(payload + 19, p->dst_ext);
    wr64(payload + 27, p->tc_ext);
    const uint8_t aps_header[2] = {0x01 /* command, unicast */, p->aps_counter};
    nwk_aux_t aux = {.key_id = 2, .extended_nonce = true, .frame_counter = p->aps_frame_counter,
                     .source = p->tc_ext};
    uint8_t ktk[16];
    zb_key_transport_key(p->tc_link_key, ktk);
    size_t m = aps_seal(ktk, aps_header, sizeof aps_header, &aux, payload, sizeof payload, out + n, cap - n);
    volatile uint8_t *wipe = ktk;
    for (unsigned i = 0; i < 16; i++) wipe[i] = 0;
    wipe = payload;
    for (unsigned i = 0; i < sizeof payload; i++) wipe[i] = 0;
    return m ? n + m : 0;
}
