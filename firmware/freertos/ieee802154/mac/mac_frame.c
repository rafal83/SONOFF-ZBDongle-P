/* SPDX-License-Identifier: MIT
 * Written from IEEE 802.15.4-2006 clause 7.2 and 802.15.4-2015 7.2.1.5
 * (PAN ID compression table). Zigbee beacon payload per Zigbee spec 3.6.7.
 */
#include "mac_frame.h"

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
static void wr64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8u * i)); }

/* Which PAN IDs are present, per frame version. Returns false if reserved. */
static bool pan_presence(uint8_t version, uint8_t dst_mode, uint8_t src_mode, bool comp,
                         bool *dst_pan, bool *src_pan) {
    bool d = dst_mode != MAC_ADDR_NONE, s = src_mode != MAC_ADDR_NONE;
    if (version < 2) {
        /* 2003/2006: compression only meaningful with both addresses. */
        if (comp && !(d && s)) return false;
        *dst_pan = d;
        *src_pan = s && !comp;
        return true;
    }
    /* 2015 Table 7-2 (no extended+short ambiguity handling beyond the table). */
    if (!d && !s) { *dst_pan = comp; *src_pan = false; return true; }
    if (d && !s) { *dst_pan = !comp; *src_pan = false; return true; }
    if (!d && s) { *dst_pan = false; *src_pan = !comp; return true; }
    if (dst_mode == MAC_ADDR_EXT && src_mode == MAC_ADDR_EXT) {
        *dst_pan = !comp;
        *src_pan = false;
        return true;
    }
    *dst_pan = true;
    *src_pan = !comp;
    return true;
}

static size_t addr_len(uint8_t mode) { return mode == MAC_ADDR_SHORT ? 2u : mode == MAC_ADDR_EXT ? 8u : 0u; }

int mac_frame_parse(const uint8_t *p, size_t len, mac_frame_t *f) {
    if (!p || !f) return MAC_ERR_PARAM;
    *f = (mac_frame_t){0};
    if (len < 2) return MAC_ERR_TRUNCATED;
    uint16_t fcf = rd16(p);
    f->type = fcf & 7u;
    f->security = (fcf >> 3) & 1u;
    f->pending = (fcf >> 4) & 1u;
    f->ack_request = (fcf >> 5) & 1u;
    f->pan_id_compression = (fcf >> 6) & 1u;
    f->seq_suppressed = (fcf >> 8) & 1u;
    bool ie_present = (fcf >> 9) & 1u;
    f->dst_mode = (fcf >> 10) & 3u;
    f->version = (fcf >> 12) & 3u;
    f->src_mode = (fcf >> 14) & 3u;
    if (f->type > MAC_TYPE_COMMAND) return MAC_ERR_UNSUPPORTED;   /* multipurpose/frak/extended */
    if (f->version == 3) return MAC_ERR_RESERVED;
    if (f->dst_mode == 1 || f->src_mode == 1) return MAC_ERR_RESERVED;
    if (f->version < 2 && (f->seq_suppressed || ie_present || (fcf & 0x0080u))) return MAC_ERR_RESERVED;
    if (ie_present) return MAC_ERR_UNSUPPORTED;
    size_t pos = 2;
    if (!f->seq_suppressed) {
        if (len < pos + 1) return MAC_ERR_TRUNCATED;
        f->seq = p[pos++];
    }
    if (!pan_presence(f->version, f->dst_mode, f->src_mode, f->pan_id_compression,
                      &f->has_dst_pan, &f->has_src_pan)) return MAC_ERR_RESERVED;
    size_t need = (f->has_dst_pan ? 2u : 0u) + addr_len(f->dst_mode) +
                  (f->has_src_pan ? 2u : 0u) + addr_len(f->src_mode);
    if (len < pos + need) return MAC_ERR_TRUNCATED;
    if (f->has_dst_pan) { f->dst_pan = rd16(p + pos); pos += 2; }
    if (f->dst_mode == MAC_ADDR_SHORT) { f->dst_short = rd16(p + pos); pos += 2; }
    else if (f->dst_mode == MAC_ADDR_EXT) { f->dst_ext = rd64(p + pos); pos += 8; }
    if (f->has_src_pan) { f->src_pan = rd16(p + pos); pos += 2; }
    else if (f->src_mode != MAC_ADDR_NONE && f->has_dst_pan) f->src_pan = f->dst_pan;
    if (f->src_mode == MAC_ADDR_SHORT) { f->src_short = rd16(p + pos); pos += 2; }
    else if (f->src_mode == MAC_ADDR_EXT) { f->src_ext = rd64(p + pos); pos += 8; }
    if (f->security) {
        if (f->version == 0) return MAC_ERR_RESERVED;
        if (len < pos + 1) return MAC_ERR_TRUNCATED;
        uint8_t sc = p[pos++];
        f->sec_level = sc & 7u;
        f->key_id_mode = (sc >> 3) & 3u;
        f->frame_counter_suppressed = f->version >= 2 && ((sc >> 5) & 1u);
        if (f->version < 2 && (sc & 0xE0u)) return MAC_ERR_RESERVED;
        if (!f->frame_counter_suppressed) {
            if (len < pos + 4) return MAC_ERR_TRUNCATED;
            f->frame_counter = rd32(p + pos);
            pos += 4;
        }
        static const uint8_t source_len[4] = {0, 0, 4, 8};
        size_t kid = f->key_id_mode ? source_len[f->key_id_mode] + 1u : 0u;
        if (len < pos + kid) return MAC_ERR_TRUNCATED;
        if (f->key_id_mode == 2) f->key_source = rd32(p + pos);
        else if (f->key_id_mode == 3) f->key_source = rd64(p + pos);
        if (f->key_id_mode) f->key_index = p[pos + kid - 1u];
        pos += kid;
    }
    f->header_len = pos;
    f->payload = p + pos;
    f->payload_len = len - pos;
    if (f->type == MAC_TYPE_ACK) {
        if (f->dst_mode || f->src_mode || f->payload_len) {
            /* 2006 imm-ACK has no addresses or payload. */
            if (f->version < 2) return MAC_ERR_RESERVED;
        }
    } else if (f->type == MAC_TYPE_COMMAND && f->payload_len < 1 && !f->security) {
        return MAC_ERR_TRUNCATED;
    }
    return MAC_OK;
}

size_t mac_frame_build(const mac_frame_t *f, const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t cap) {
    if (!f || !out || f->security || f->type > MAC_TYPE_COMMAND || f->version > 1 ||
        f->dst_mode == 1 || f->src_mode == 1 || f->dst_mode > 3 || f->src_mode > 3 ||
        (payload_len && !payload)) return 0;
    bool dst_pan, src_pan;
    if (!pan_presence(f->version, f->dst_mode, f->src_mode, f->pan_id_compression, &dst_pan, &src_pan)) return 0;
    size_t len = 3 + (dst_pan ? 2u : 0u) + addr_len(f->dst_mode) + (src_pan ? 2u : 0u) +
                 addr_len(f->src_mode) + payload_len;
    if (len > cap || len > MAC_MAX_MPDU) return 0;
    uint16_t fcf = (uint16_t)(f->type | (f->pending << 4) | (f->ack_request << 5) |
                              (f->pan_id_compression << 6) | (f->dst_mode << 10) |
                              (f->version << 12) | (f->src_mode << 14));
    size_t pos = 0;
    wr16(out, fcf);
    out[2] = f->seq;
    pos = 3;
    if (dst_pan) { wr16(out + pos, f->dst_pan); pos += 2; }
    if (f->dst_mode == MAC_ADDR_SHORT) { wr16(out + pos, f->dst_short); pos += 2; }
    else if (f->dst_mode == MAC_ADDR_EXT) { wr64(out + pos, f->dst_ext); pos += 8; }
    if (src_pan) { wr16(out + pos, f->src_pan); pos += 2; }
    if (f->src_mode == MAC_ADDR_SHORT) { wr16(out + pos, f->src_short); pos += 2; }
    else if (f->src_mode == MAC_ADDR_EXT) { wr64(out + pos, f->src_ext); pos += 8; }
    for (size_t i = 0; i < payload_len; i++) out[pos + i] = payload[i];
    return len;
}

uint16_t mac_fcs(const uint8_t *data, size_t len) {
    /* ITU-T CRC-16 (x^16+x^12+x^5+1), LSB first, init 0: 802.15.4 FCS. */
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++) crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

size_t mac_build_zigbee_beacon(const mac_zigbee_beacon_t *b, uint8_t *out, size_t cap) {
    if (!b || b->device_depth > 15 || b->stack_profile > 15 || b->protocol_version > 15) return 0;
    uint8_t payload[4 + 15];
    /* Superframe spec: BO=15, SO=15, final CAP slot=15, PAN coordinator if
     * depth 0, association permit. No GTS, no pending addresses. */
    uint16_t sf = 0x0FFFu | (b->device_depth == 0 ? (1u << 14) : 0u) |
                  (b->association_permit ? (1u << 15) : 0u);
    payload[0] = (uint8_t)sf;
    payload[1] = (uint8_t)(sf >> 8);
    payload[2] = 0; /* GTS */
    payload[3] = 0; /* pending addresses */
    payload[4] = 0; /* Zigbee protocol id */
    payload[5] = (uint8_t)(b->stack_profile | (b->protocol_version << 4));
    payload[6] = (uint8_t)((b->router_capacity ? 0x04u : 0u) | (b->device_depth << 3) |
                           (b->end_device_capacity ? 0x80u : 0u));
    for (unsigned i = 0; i < 8; i++) payload[7 + i] = b->ext_pan_id[i];
    payload[15] = payload[16] = payload[17] = 0xFF; /* TX offset: none */
    payload[18] = b->update_id;
    mac_frame_t f = {0};
    f.type = MAC_TYPE_BEACON;
    f.seq = b->seq;
    f.src_mode = MAC_ADDR_SHORT;
    f.src_pan = b->pan_id;
    f.src_short = b->short_addr;
    return mac_frame_build(&f, payload, sizeof payload, out, cap);
}

int mac_parse_zigbee_beacon(const mac_frame_t *f, mac_zigbee_beacon_t *b) {
    if (!f || !b) return MAC_ERR_PARAM;
    if (f->type != MAC_TYPE_BEACON || f->src_mode == MAC_ADDR_NONE) return MAC_ERR_UNSUPPORTED;
    if (f->payload_len < 4) return MAC_ERR_TRUNCATED;
    const uint8_t *p = f->payload;
    uint8_t gts = p[2] & 7u;
    size_t pos = 3 + (gts ? 1u + 3u * gts : 0u);
    if (f->payload_len < pos + 1) return MAC_ERR_TRUNCATED;
    uint8_t pend = p[pos++];
    pos += 2u * (pend & 7u) + 8u * ((pend >> 4) & 7u);
    if (f->payload_len < pos + 15) return MAC_ERR_TRUNCATED;
    if (p[pos] != 0) return MAC_ERR_UNSUPPORTED; /* not a Zigbee beacon */
    uint16_t sf = rd16(p);
    *b = (mac_zigbee_beacon_t){0};
    b->pan_id = f->src_pan;
    b->short_addr = f->src_short;
    b->seq = f->seq;
    b->association_permit = (sf >> 15) & 1u;
    b->stack_profile = p[pos + 1] & 0x0Fu;
    b->protocol_version = p[pos + 1] >> 4;
    b->router_capacity = (p[pos + 2] >> 2) & 1u;
    b->device_depth = (p[pos + 2] >> 3) & 0x0Fu;
    b->end_device_capacity = (p[pos + 2] >> 7) & 1u;
    for (unsigned i = 0; i < 8; i++) b->ext_pan_id[i] = p[pos + 3 + i];
    b->update_id = p[pos + 14];
    return MAC_OK;
}

size_t mac_build_assoc_response(uint8_t seq, uint16_t pan_id, uint64_t dst_ext, uint64_t src_ext,
                                uint16_t short_addr, uint8_t status, uint8_t *out, size_t cap) {
    mac_frame_t f = {0};
    f.type = MAC_TYPE_COMMAND;
    f.ack_request = true;
    f.pan_id_compression = true;
    f.seq = seq;
    f.dst_mode = MAC_ADDR_EXT;
    f.src_mode = MAC_ADDR_EXT;
    f.dst_pan = pan_id;
    f.dst_ext = dst_ext;
    f.src_ext = src_ext;
    const uint8_t payload[4] = {MAC_CMD_ASSOC_RESPONSE, (uint8_t)short_addr, (uint8_t)(short_addr >> 8), status};
    return mac_frame_build(&f, payload, sizeof payload, out, cap);
}
