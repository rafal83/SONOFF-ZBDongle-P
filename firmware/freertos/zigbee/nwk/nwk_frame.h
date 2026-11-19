/* SPDX-License-Identifier: MIT
 * Zigbee PRO NWK header codec (Zigbee spec 3.3.1) and NWK auxiliary security
 * header (4.5.1). The header layout matches what firmware/src/core_security.c
 * validates (protocol version 2, optional IEEE addresses, multicast control,
 * source route subframe).
 */
#ifndef ZB_NWK_FRAME_H
#define ZB_NWK_FRAME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { NWK_TYPE_DATA = 0, NWK_TYPE_COMMAND = 1, NWK_TYPE_INTERPAN = 3 };
enum {
    NWK_OK = 0,
    NWK_ERR_TRUNCATED = -1,
    NWK_ERR_RESERVED = -2,
    NWK_ERR_UNSUPPORTED = -3,
    NWK_ERR_PARAM = -4,
};
#define NWK_PROTOCOL_VERSION 2u
#define NWK_MAX_RELAYS 30u
#define NWK_BCAST_ALL 0xFFFFu
#define NWK_BCAST_RX_ON 0xFFFDu
#define NWK_BCAST_ROUTERS 0xFFFCu
#define NWK_BCAST_LOW_POWER_ROUTERS 0xFFFBu

typedef struct {
    uint8_t type, protocol_version, discover_route;
    bool multicast, security, source_route, end_device_initiator;
    bool has_dst_ieee, has_src_ieee;
    uint16_t dst, src;
    uint8_t radius, seq;
    uint64_t dst_ieee, src_ieee;
    uint8_t multicast_control;
    uint8_t relay_count, relay_index;
    uint16_t relays[NWK_MAX_RELAYS];
    size_t header_len;
    const uint8_t *payload;      /* after the header (aux header if secured) */
    size_t payload_len;
} nwk_header_t;

int nwk_parse(const uint8_t *frame, size_t len, nwk_header_t *out);
size_t nwk_build(const nwk_header_t *h, uint8_t *out, size_t cap);

/* NWK auxiliary security header. On air the level field is 0 (spec 4.3.1.1);
 * the real level (5, ENC-MIC-32) is substituted for nonce/AAD computation. */
typedef struct {
    uint8_t level, key_id;
    bool extended_nonce;
    uint32_t frame_counter;
    uint64_t source;
    uint8_t key_seq;
    size_t len;
} nwk_aux_t;
int nwk_aux_parse(const uint8_t *p, size_t len, nwk_aux_t *out);
size_t nwk_aux_build(const nwk_aux_t *a, uint8_t *out, size_t cap);
/* Security nonce: source IEEE (LE) | frame counter (LE) | security control. */
void nwk_nonce(uint64_t source, uint32_t counter, uint8_t security_control, uint8_t nonce[13]);
bool nwk_is_broadcast(uint16_t address);
#endif
