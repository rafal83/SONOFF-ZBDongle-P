/* SPDX-License-Identifier: MIT */
#include "nwk_frame.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8u * i)); }
static void wr64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8u * i)); }

bool nwk_is_broadcast(uint16_t a) { return a >= 0xFFF8u; }

int nwk_parse(const uint8_t *p, size_t len, nwk_header_t *h) {
    if (!p || !h) return NWK_ERR_PARAM;
    *h = (nwk_header_t){0};
    if (len < 8) return NWK_ERR_TRUNCATED;
    uint16_t fc = rd16(p);
    h->type = fc & 3u;
    h->protocol_version = (fc >> 2) & 0x0Fu;
    h->discover_route = (fc >> 6) & 3u;
    h->multicast = (fc >> 8) & 1u;
    h->security = (fc >> 9) & 1u;
    h->source_route = (fc >> 10) & 1u;
    h->has_dst_ieee = (fc >> 11) & 1u;
    h->has_src_ieee = (fc >> 12) & 1u;
    h->end_device_initiator = (fc >> 13) & 1u;
    if (h->type == 2) return NWK_ERR_RESERVED;
    if (h->type == NWK_TYPE_INTERPAN) return NWK_ERR_UNSUPPORTED;  /* handled by APS inter-PAN path */
    if (h->protocol_version != NWK_PROTOCOL_VERSION) return NWK_ERR_UNSUPPORTED;
    if (fc & 0xC000u) return NWK_ERR_RESERVED;
    h->dst = rd16(p + 2);
    h->src = rd16(p + 4);
    h->radius = p[6];
    h->seq = p[7];
    size_t pos = 8;
    if (h->has_dst_ieee) {
        if (len < pos + 8) return NWK_ERR_TRUNCATED;
        h->dst_ieee = rd64(p + pos);
        pos += 8;
    }
    if (h->has_src_ieee) {
        if (len < pos + 8) return NWK_ERR_TRUNCATED;
        h->src_ieee = rd64(p + pos);
        pos += 8;
    }
    if (h->multicast) {
        if (len < pos + 1) return NWK_ERR_TRUNCATED;
        h->multicast_control = p[pos++];
    }
    if (h->source_route) {
        if (len < pos + 2) return NWK_ERR_TRUNCATED;
        h->relay_count = p[pos];
        h->relay_index = p[pos + 1];
        pos += 2;
        /* relay_index points at the next relay to use; it may equal
         * count - 1 down to 0 while travelling, never >= count. */
        if (!h->relay_count || h->relay_count > NWK_MAX_RELAYS || h->relay_index >= h->relay_count)
            return NWK_ERR_RESERVED;
        if (len < pos + 2u * h->relay_count) return NWK_ERR_TRUNCATED;
        for (unsigned i = 0; i < h->relay_count; i++) h->relays[i] = rd16(p + pos + 2u * i);
        pos += 2u * h->relay_count;
    }
    h->header_len = pos;
    h->payload = p + pos;
    h->payload_len = len - pos;
    if (h->type == NWK_TYPE_COMMAND && !h->security && h->payload_len < 1) return NWK_ERR_TRUNCATED;
    return NWK_OK;
}

size_t nwk_build(const nwk_header_t *h, uint8_t *out, size_t cap) {
    if (!h || !out || h->type > 1 || h->discover_route > 3 ||
        (h->source_route && (!h->relay_count || h->relay_count > NWK_MAX_RELAYS ||
                             h->relay_index >= h->relay_count))) return 0;
    size_t len = 8 + (h->has_dst_ieee ? 8u : 0u) + (h->has_src_ieee ? 8u : 0u) +
                 (h->multicast ? 1u : 0u) + (h->source_route ? 2u + 2u * h->relay_count : 0u);
    if (len > cap) return 0;
    uint16_t fc = (uint16_t)(h->type | (NWK_PROTOCOL_VERSION << 2) | (h->discover_route << 6) |
                             (h->multicast << 8) | (h->security << 9) | (h->source_route << 10) |
                             (h->has_dst_ieee << 11) | (h->has_src_ieee << 12) |
                             (h->end_device_initiator << 13));
    wr16(out, fc);
    wr16(out + 2, h->dst);
    wr16(out + 4, h->src);
    out[6] = h->radius;
    out[7] = h->seq;
    size_t pos = 8;
    if (h->has_dst_ieee) { wr64(out + pos, h->dst_ieee); pos += 8; }
    if (h->has_src_ieee) { wr64(out + pos, h->src_ieee); pos += 8; }
    if (h->multicast) out[pos++] = h->multicast_control;
    if (h->source_route) {
        out[pos++] = h->relay_count;
        out[pos++] = h->relay_index;
        for (unsigned i = 0; i < h->relay_count; i++) { wr16(out + pos, h->relays[i]); pos += 2; }
    }
    return pos;
}

int nwk_aux_parse(const uint8_t *p, size_t len, nwk_aux_t *a) {
    if (!p || !a) return NWK_ERR_PARAM;
    *a = (nwk_aux_t){0};
    if (len < 5) return NWK_ERR_TRUNCATED;
    uint8_t sc = p[0];
    a->level = sc & 7u;
    a->key_id = (sc >> 3) & 3u;
    a->extended_nonce = (sc >> 5) & 1u;
    if (sc & 0xC0u) return NWK_ERR_RESERVED;
    a->frame_counter = rd32(p + 1);
    size_t pos = 5;
    if (a->extended_nonce) {
        if (len < pos + 8) return NWK_ERR_TRUNCATED;
        a->source = rd64(p + pos);
        pos += 8;
    }
    if (a->key_id == 1) {
        if (len < pos + 1) return NWK_ERR_TRUNCATED;
        a->key_seq = p[pos++];
    }
    a->len = pos;
    return NWK_OK;
}

size_t nwk_aux_build(const nwk_aux_t *a, uint8_t *out, size_t cap) {
    if (!a || !out || a->level > 7 || a->key_id > 3) return 0;
    size_t len = 5 + (a->extended_nonce ? 8u : 0u) + (a->key_id == 1 ? 1u : 0u);
    if (len > cap) return 0;
    out[0] = (uint8_t)(a->level | (a->key_id << 3) | (a->extended_nonce << 5));
    wr32(out + 1, a->frame_counter);
    size_t pos = 5;
    if (a->extended_nonce) { wr64(out + pos, a->source); pos += 8; }
    if (a->key_id == 1) out[pos++] = a->key_seq;
    return pos;
}

void nwk_nonce(uint64_t source, uint32_t counter, uint8_t sc, uint8_t nonce[13]) {
    wr64(nonce, source);
    wr32(nonce + 8, counter);
    nonce[12] = sc;
}
