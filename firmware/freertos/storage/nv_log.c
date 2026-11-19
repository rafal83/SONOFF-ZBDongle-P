/* SPDX-License-Identifier: MIT */
#include "nv_log.h"

#define MAGIC 0x564E5246u          /* "FRNV" */
#define VERSION 1u
#define COMPLETE_DONE 0x00000000u
#define TOMBSTONE 0x8000u
#define REC_HDR 8u

static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

uint32_t nv_log_crc32(uint32_t crc, const void *data, size_t length) {
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < length; i++) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint16_t hcrc(uint16_t id, uint16_t len) {
    uint8_t b[4] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    return (uint16_t)(nv_log_crc32(0x5A5Au, b, 4) & 0xFFFFu);
}

static uint32_t bank_addr(const nv_log_t *l, unsigned bank) { return l->base + bank * l->bank_size; }

static bool erase_bank(nv_log_t *l, unsigned bank) {
    for (uint32_t off = 0; off < l->bank_size; off += l->sector_size) {
        if (!l->ops.erase_sector(l->ops.ctx, bank_addr(l, bank) + off)) { l->stats.erase_failures++; return false; }
    }
    return true;
}

static bool program(nv_log_t *l, uint32_t addr, const void *data, size_t len) {
    if (l->ops.program(l->ops.ctx, addr, data, len)) return true;
    l->stats.program_failures++;
    return false;
}

typedef struct { uint32_t magic; uint16_t version, reserved; uint32_t generation, complete; } bank_header_t;

static bool read_header(nv_log_t *l, unsigned bank, bank_header_t *h) {
    uint8_t b[16];
    if (!l->ops.read(l->ops.ctx, bank_addr(l, bank), b, 16)) return false;
    h->magic = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    h->version = (uint16_t)(b[4] | b[5] << 8);
    h->generation = (uint32_t)b[8] | (uint32_t)b[9] << 8 | (uint32_t)b[10] << 16 | (uint32_t)b[11] << 24;
    h->complete = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
    return h->magic == MAGIC && h->version == VERSION && h->complete == COMPLETE_DONE;
}

static bool write_header(nv_log_t *l, unsigned bank, uint32_t generation, bool complete) {
    uint8_t b[16];
    uint32_t words[4] = {MAGIC, VERSION | 0xFFFF0000u, generation, 0xFFFFFFFFu};
    for (unsigned i = 0; i < 4; i++)
        for (unsigned k = 0; k < 4; k++) b[4 * i + k] = (uint8_t)(words[i] >> (8 * k));
    if (!program(l, bank_addr(l, bank), b, 12)) return false;
    if (!complete) return true;
    const uint8_t zero[4] = {0, 0, 0, 0};
    return program(l, bank_addr(l, bank) + 12, zero, 4);
}

/* Read and validate the record at `off`. Returns: 1 valid, 0 end of log,
 * -1 torn record (skippable, *next set), -2 torn header (stop). */
static int read_record(nv_log_t *l, unsigned bank, uint32_t off, uint16_t *id, uint16_t *len,
                       uint32_t *next, uint8_t *data) {
    uint8_t h[REC_HDR];
    if (off + REC_HDR > l->bank_size) return 0;
    if (!l->ops.read(l->ops.ctx, bank_addr(l, bank) + off, h, REC_HDR)) return -2;
    bool blank = true;
    for (unsigned i = 0; i < REC_HDR; i++) blank &= h[i] == 0xFF;
    if (blank) return 0;
    *id = (uint16_t)(h[0] | h[1] << 8);
    *len = (uint16_t)(h[2] | h[3] << 8);
    uint16_t stored = (uint16_t)(h[4] | h[5] << 8);
    uint16_t plen = *len & (uint16_t)~TOMBSTONE;
    if (stored != hcrc(*id, *len) || plen > NV_LOG_MAX_RECORD || *id >= NV_LOG_MAX_IDS) return -2;
    uint32_t end = off + REC_HDR + align4(plen) + 4u;
    if (end > l->bank_size) return -2;
    *next = end;
    uint8_t *d = data ? data : l->tmp;
    uint8_t t[4];
    if (!l->ops.read(l->ops.ctx, bank_addr(l, bank) + off + REC_HDR, d, plen) ||
        !l->ops.read(l->ops.ctx, bank_addr(l, bank) + end - 4u, t, 4)) return -2;
    uint32_t crc = nv_log_crc32(0, h, 4);
    crc = nv_log_crc32(crc, d, plen);
    uint32_t trailer = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
    return trailer == crc ? 1 : -1;
}

static bool scan(nv_log_t *l) {
    for (unsigned i = 0; i < NV_LOG_MAX_IDS; i++) l->index[i] = 0;
    uint32_t off = NV_LOG_HEADER;
    l->full = false;
    for (;;) {
        uint16_t id, len;
        uint32_t next;
        int r = read_record(l, l->active, off, &id, &len, &next, 0);
        if (r == 0) break;
        if (r == -2) { l->stats.torn_headers++; l->full = true; break; }
        if (r == -1) { l->stats.torn_records++; off = next; continue; }
        l->index[id] = (len & TOMBSTONE) ? 0 : (uint16_t)(off / 4u);
        off = next;
    }
    l->write_offset = off;
    return true;
}

bool nv_log_format(nv_log_t *l) {
    l->mounted = false;
    if (!l->sector_size || !erase_bank(l, 0) || !erase_bank(l, 1)) { l->mount_status = NV_LOG_ERR_IO; return false; }
    l->active = 0;
    l->generation = 1;
    if (!write_header(l, 0, 1, true)) { l->mount_status = NV_LOG_ERR_IO; return false; }
    l->mounted = scan(l);
    l->mount_status = l->mounted ? NV_LOG_FORMATTED : NV_LOG_ERR_IO;
    return l->mounted;
}

typedef enum { BANK_BLANK, BANK_UNCOMMITTED_EMPTY, BANK_UNCOMMITTED_DATA, BANK_VALID, BANK_CORRUPT, BANK_IO } bank_state_t;

/* Classify a bank whose header is not a valid committed one. */
static bank_state_t bank_state(nv_log_t *l, unsigned bank) {
    uint8_t b[16];
    if (!l->ops.read(l->ops.ctx, bank_addr(l, bank), b, 16)) return BANK_IO;
    uint32_t magic = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    uint16_t version = (uint16_t)(b[4] | b[5] << 8);
    uint32_t complete = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
    bool header_blank = true;
    for (unsigned i = 0; i < 16; i++) header_blank &= b[i] == 0xFF;
    bool uncommitted = magic == MAGIC && version == VERSION && complete == 0xFFFFFFFFu;
    if (!header_blank && !uncommitted) return BANK_CORRUPT;
    /* The rest of the bank must be erased for "blank" / "uncommitted empty". */
    for (uint32_t off = NV_LOG_HEADER; off < l->bank_size; off += sizeof l->tmp) {
        uint32_t n = l->bank_size - off < sizeof l->tmp ? l->bank_size - off : (uint32_t)sizeof l->tmp;
        if (!l->ops.read(l->ops.ctx, bank_addr(l, bank) + off, l->tmp, n)) return BANK_IO;
        for (uint32_t i = 0; i < n; i++) {
            if (l->tmp[i] != 0xFF) return header_blank ? BANK_CORRUPT : BANK_UNCOMMITTED_DATA;
        }
    }
    return header_blank ? BANK_BLANK : BANK_UNCOMMITTED_EMPTY;
}

bool nv_log_mount(nv_log_t *l, const nv_flash_ops_t *ops, uint32_t base, uint32_t bank_size, uint32_t sector_size) {
    *l = (nv_log_t){0};
    l->ops = *ops;
    l->base = base;
    l->bank_size = bank_size;
    l->sector_size = sector_size;
    l->mount_status = NV_LOG_ERR_PARAM;
    if (!sector_size || bank_size % sector_size || bank_size / 4u > 0xFFFFu || bank_size < 2u * NV_LOG_HEADER)
        return false;
    l->stats.mounts++;
    bank_header_t h0, h1;
    bool v0 = read_header(l, 0, &h0), v1 = read_header(l, 1, &h1);
    if (!v0 && !v1) {
        bank_state_t s0 = bank_state(l, 0), s1 = bank_state(l, 1);
        if (s0 == BANK_IO || s1 == BANK_IO) { l->mount_status = NV_LOG_ERR_IO; return false; }
        bool empty0 = s0 == BANK_BLANK || s0 == BANK_UNCOMMITTED_EMPTY;
        bool empty1 = s1 == BANK_BLANK || s1 == BANK_UNCOMMITTED_EMPTY;
        if (empty0 && empty1) return nv_log_format(l);   /* no committed data can be lost */
        l->mount_status = NV_LOG_ERR_CORRUPT;            /* keep everything for an explicit recovery */
        return false;
    }
    /* Highest complete generation wins (wrap-safe comparison). */
    if (v0 && (!v1 || (int32_t)(h0.generation - h1.generation) > 0)) { l->active = 0; l->generation = h0.generation; }
    else { l->active = 1; l->generation = h1.generation; }
    l->mounted = scan(l);
    l->mount_status = l->mounted ? NV_LOG_MOUNTED : NV_LOG_ERR_IO;
    return l->mounted;
}

unsigned nv_log_salvage(nv_log_t *l, uint16_t id, nv_log_salvage_fn fn, void *ctx) {
    unsigned n = 0;
    if (!l->ops.read || l->bank_size < 2u * NV_LOG_HEADER || id >= NV_LOG_MAX_IDS) return 0;
    for (unsigned bank = 0; bank < 2; bank++) {
        uint32_t off = NV_LOG_HEADER;
        for (;;) {
            uint16_t rid, len;
            uint32_t next;
            int r = read_record(l, bank, off, &rid, &len, &next, l->tmp);
            if (r == 0 || r == -2) break;           /* end of log / unknown length */
            if (r == 1 && rid == id && !(len & TOMBSTONE)) { fn(ctx, l->tmp, len); n++; }
            off = next;
        }
    }
    return n;
}

bool nv_log_read(nv_log_t *l, uint16_t id, void *buffer, uint16_t cap, uint16_t *len) {
    if (!l->mounted || id >= NV_LOG_MAX_IDS || !l->index[id]) return false;
    uint16_t rid, rlen;
    uint32_t next;
    if (read_record(l, l->active, (uint32_t)l->index[id] * 4u, &rid, &rlen, &next, l->tmp) != 1 || rid != id)
        return false;
    uint8_t *out = buffer;
    if (out != l->tmp) for (uint16_t i = 0; i < rlen && i < cap; i++) out[i] = l->tmp[i];
    if (len) *len = rlen;
    return true;
}

static bool append(nv_log_t *l, unsigned bank, uint32_t *off, uint16_t id, uint16_t len, const uint8_t *data) {
    uint16_t plen = len & (uint16_t)~TOMBSTONE;
    uint32_t need = REC_HDR + align4(plen) + 4u;
    if (*off + need > l->bank_size) return false;
    uint16_t hc = hcrc(id, len);
    uint8_t h[REC_HDR] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)len, (uint8_t)(len >> 8),
                          (uint8_t)hc, (uint8_t)(hc >> 8), 0xFF, 0xFF};
    uint32_t a = bank_addr(l, bank) + *off;
    if (!program(l, a, h, REC_HDR)) return false;
    if (plen && !program(l, a + REC_HDR, data, plen)) return false;
    uint32_t crc = nv_log_crc32(nv_log_crc32(0, h, 4), data, plen);
    uint8_t t[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
    if (!program(l, a + need - 4u, t, 4)) return false;
    *off += need;
    return true;
}

static bool compact(nv_log_t *l) {
    unsigned target = 1u - l->active;
    if (!erase_bank(l, target)) return false;
    uint32_t gen = l->generation + 1u;
    if (!write_header(l, target, gen, false)) return false;
    uint32_t off = NV_LOG_HEADER;
    uint16_t *new_index = l->scratch_index;
    for (unsigned id = 0; id < NV_LOG_MAX_IDS; id++) {
        new_index[id] = 0;
        if (!l->index[id]) continue;
        uint16_t len;
        if (!nv_log_read(l, (uint16_t)id, l->tmp2, sizeof l->tmp2, &len)) continue;
        uint32_t at = off;
        if (!append(l, target, &off, (uint16_t)id, len, l->tmp2)) return false;
        new_index[id] = (uint16_t)(at / 4u);
    }
    const uint8_t zero[4] = {0, 0, 0, 0};
    if (!program(l, bank_addr(l, target) + 12u, zero, 4)) return false;   /* commit point */
    l->active = (uint8_t)target;
    l->generation = gen;
    l->write_offset = off;
    l->full = false;
    for (unsigned id = 0; id < NV_LOG_MAX_IDS; id++) l->index[id] = new_index[id];
    l->stats.compactions++;
    return true;
}

static bool put(nv_log_t *l, uint16_t id, uint16_t len, const uint8_t *data) {
    uint32_t need = REC_HDR + align4(len & (uint16_t)~TOMBSTONE) + 4u;
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (!l->full && l->write_offset + need <= l->bank_size) {
            uint32_t at = l->write_offset, off = at;
            if (append(l, l->active, &off, id, len, data)) {
                l->write_offset = off;
                l->index[id] = (len & TOMBSTONE) ? 0 : (uint16_t)(at / 4u);
                l->stats.writes++;
                return true;
            }
            /* Program failure: the area after `at` may be partly written and
             * must never be appended to again. Move to the other bank. */
            l->full = true;
        }
        if (!compact(l)) return false;
    }
    return false;
}

bool nv_log_write(nv_log_t *l, uint16_t id, const void *data, uint16_t len) {
    if (!l->mounted || id >= NV_LOG_MAX_IDS || len > NV_LOG_MAX_RECORD || (len && !data)) return false;
    uint16_t clen;
    if (nv_log_read(l, id, l->tmp2, sizeof l->tmp2, &clen) && clen == len) {
        const uint8_t *d = data;
        bool same = true;
        for (uint16_t i = 0; i < len; i++) same &= l->tmp2[i] == d[i];
        if (same) return true;     /* no wear for identical content */
    }
    return put(l, id, len, data);
}

bool nv_log_delete(nv_log_t *l, uint16_t id) {
    if (!l->mounted || id >= NV_LOG_MAX_IDS) return false;
    if (!l->index[id]) return true;
    return put(l, id, TOMBSTONE, 0);
}
