/* SPDX-License-Identifier: MIT
 * Field presence (Zigbee spec 2.2.5.1.1):
 *  - destination endpoint: data/ACK (non ack-format) with unicast/broadcast
 *  - group address: delivery mode group
 *  - cluster + profile: data frames, and ACK frames without ack format
 *  - source endpoint: as destination endpoint, also present for group
 *  - extended header: fragmentation control (+ block number, + ACK bitfield)
 */
#include "aps_frame.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void presence(const aps_header_t *h, bool *dst_ep, bool *group, bool *cluster, bool *src_ep) {
    bool endpoints = (h->type == APS_TYPE_DATA) || (h->type == APS_TYPE_ACK && !h->ack_format);
    *group = h->type == APS_TYPE_DATA && h->delivery == APS_DELIVERY_GROUP;
    *dst_ep = endpoints && !*group;
    *cluster = endpoints;
    *src_ep = endpoints;
}

int aps_parse(const uint8_t *p, size_t len, aps_header_t *h) {
    if (!p || !h) return APS_ERR_PARAM;
    *h = (aps_header_t){0};
    if (len < 2) return APS_ERR_TRUNCATED;
    uint8_t fc = p[0];
    h->type = fc & 3u;
    h->delivery = (fc >> 2) & 3u;
    h->ack_format = (fc >> 4) & 1u;
    h->security = (fc >> 5) & 1u;
    h->ack_request = (fc >> 6) & 1u;
    h->extended = (fc >> 7) & 1u;
    if (h->type == APS_TYPE_INTERPAN) return APS_ERR_UNSUPPORTED;
    if (h->delivery == 1) return APS_ERR_RESERVED; /* indirect: removed in Zigbee 2007+ */
    if (h->type == APS_TYPE_COMMAND && h->delivery == APS_DELIVERY_GROUP) return APS_ERR_RESERVED;
    if (h->type == APS_TYPE_ACK && h->delivery == APS_DELIVERY_GROUP) return APS_ERR_RESERVED;
    bool dst_ep, group, cluster, src_ep;
    presence(h, &dst_ep, &group, &cluster, &src_ep);
    size_t need = 1 + (dst_ep ? 1u : 0u) + (group ? 2u : 0u) + (cluster ? 4u : 0u) + (src_ep ? 1u : 0u) + 1u;
    if (len < need) return APS_ERR_TRUNCATED;
    size_t pos = 1;
    if (dst_ep) h->dst_endpoint = p[pos++];
    if (group) { h->group = rd16(p + pos); pos += 2; }
    if (cluster) {
        h->cluster = rd16(p + pos);
        h->profile = rd16(p + pos + 2);
        pos += 4;
    }
    if (src_ep) h->src_endpoint = p[pos++];
    h->counter = p[pos++];
    if (h->extended) {
        if (len < pos + 1) return APS_ERR_TRUNCATED;
        uint8_t ext = p[pos++];
        h->fragmentation = ext & 3u;
        if (h->fragmentation == 3 || (ext & 0xFCu)) return APS_ERR_RESERVED;
        if (h->fragmentation != APS_FRAG_NONE) {
            if (len < pos + 1) return APS_ERR_TRUNCATED;
            h->block_number = p[pos++];
            if (h->type == APS_TYPE_ACK) {
                if (len < pos + 1) return APS_ERR_TRUNCATED;
                h->ack_bitfield = p[pos++];
            }
        }
    }
    h->header_len = pos;
    h->payload = p + pos;
    h->payload_len = len - pos;
    if (h->type == APS_TYPE_COMMAND && !h->security && h->payload_len < 1) return APS_ERR_TRUNCATED;
    return APS_OK;
}

size_t aps_build(const aps_header_t *h, uint8_t *out, size_t cap) {
    if (!h || !out || h->type > 2 || h->delivery == 1 || h->delivery > 3 ||
        (h->delivery == APS_DELIVERY_GROUP && h->type != APS_TYPE_DATA) || h->fragmentation > 2) return 0;
    bool dst_ep, group, cluster, src_ep;
    presence(h, &dst_ep, &group, &cluster, &src_ep);
    size_t len = 1 + (dst_ep ? 1u : 0u) + (group ? 2u : 0u) + (cluster ? 4u : 0u) + (src_ep ? 1u : 0u) + 1u;
    if (h->extended) len += 1u + (h->fragmentation ? 1u + (h->type == APS_TYPE_ACK ? 1u : 0u) : 0u);
    if (len > cap) return 0;
    size_t pos = 0;
    out[pos++] = (uint8_t)(h->type | (h->delivery << 2) | (h->ack_format << 4) | (h->security << 5) |
                           (h->ack_request << 6) | (h->extended << 7));
    if (dst_ep) out[pos++] = h->dst_endpoint;
    if (group) { wr16(out + pos, h->group); pos += 2; }
    if (cluster) { wr16(out + pos, h->cluster); wr16(out + pos + 2, h->profile); pos += 4; }
    if (src_ep) out[pos++] = h->src_endpoint;
    out[pos++] = h->counter;
    if (h->extended) {
        out[pos++] = h->fragmentation;
        if (h->fragmentation) {
            out[pos++] = h->block_number;
            if (h->type == APS_TYPE_ACK) out[pos++] = h->ack_bitfield;
        }
    }
    return pos;
}
