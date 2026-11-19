/* SPDX-License-Identifier: MIT
 * Zigbee APS header codec (Zigbee spec 2.2.5.1), including the APS ACK
 * format, group delivery and the extended header (fragmentation fields).
 */
#ifndef ZB_APS_FRAME_H
#define ZB_APS_FRAME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { APS_TYPE_DATA = 0, APS_TYPE_COMMAND = 1, APS_TYPE_ACK = 2, APS_TYPE_INTERPAN = 3 };
enum { APS_DELIVERY_UNICAST = 0, APS_DELIVERY_BROADCAST = 2, APS_DELIVERY_GROUP = 3 };
enum {
    APS_OK = 0,
    APS_ERR_TRUNCATED = -1,
    APS_ERR_RESERVED = -2,
    APS_ERR_UNSUPPORTED = -3,
    APS_ERR_PARAM = -4,
};
enum { APS_FRAG_NONE = 0, APS_FRAG_FIRST = 1, APS_FRAG_NEXT = 2 };

typedef struct {
    uint8_t type, delivery;
    bool ack_format, security, ack_request, extended;
    uint8_t dst_endpoint, src_endpoint;
    uint16_t group, cluster, profile;
    uint8_t counter;
    uint8_t fragmentation, block_number, ack_bitfield;
    size_t header_len;
    const uint8_t *payload;
    size_t payload_len;
} aps_header_t;

int aps_parse(const uint8_t *p, size_t len, aps_header_t *out);
size_t aps_build(const aps_header_t *h, uint8_t *out, size_t cap);
#endif
