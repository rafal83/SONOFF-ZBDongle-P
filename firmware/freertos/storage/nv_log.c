/* SPDX-License-Identifier: MIT */
#include "nv_log.h"

#define MAGIC 0x564E5246u          /* "FRNV" */
#define VERSION 1u
#define COMPLETE_DONE 0x00000000u
#define TOMBSTONE 0x8000u
#define REC_HDR 8u

static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

/* CRC-32 (IEEE, reflected, as zlib), table-driven: 1 KiB of Flash, about 8x
 * faster than bit by bit (every read, idempotent-write check, mount and
 * compaction goes through it). */
static const uint32_t CRC32_TABLE[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

uint32_t nv_log_crc32(uint32_t crc, const void *data, size_t length) {
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < length; i++) crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ p[i]) & 0xFFu];
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

/* ---- storage layout migration ------------------------------------------ */

static bool ranges_overlap(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
    return a < b + blen && b < a + alen;
}

nv_log_migration_t nv_log_migrate(nv_log_t *l, const nv_flash_ops_t *ops, uint32_t old_base, uint32_t old_bank_size,
                                  uint32_t base, uint32_t bank_size, uint32_t sector_size) {
    *l = (nv_log_t){0};
    l->ops = *ops;
    l->sector_size = sector_size;
    l->mount_status = NV_LOG_ERR_PARAM;
    /* The copy goes to bank 0 of the new geometry, which must not overlap the
     * old store (still the authoritative copy until the commit). */
    if (!sector_size || bank_size % sector_size || old_bank_size % sector_size || bank_size / 4u > 0xFFFFu ||
        old_bank_size / 4u > 0xFFFFu || ranges_overlap(base, bank_size, old_base, 2u * old_bank_size))
        return NV_LOG_MIGRATION_ERROR;
    /* 1. Old store, read only: the highest complete generation, no format. */
    l->base = old_base;
    l->bank_size = old_bank_size;
    bank_header_t h0, h1;
    bool v0 = read_header(l, 0, &h0), v1 = read_header(l, 1, &h1);
    if (!v0 && !v1) return NV_LOG_MIGRATION_NONE;
    if (v0 && (!v1 || (int32_t)(h0.generation - h1.generation) > 0)) { l->active = 0; l->generation = h0.generation; }
    else { l->active = 1; l->generation = h1.generation; }
    if (!scan(l)) return NV_LOG_MIGRATION_ERROR;
    unsigned old_active = l->active;
    for (unsigned id = 0; id < NV_LOG_MAX_IDS; id++) l->scratch_index[id] = l->index[id];
    /* 2. Copy every live record into the new bank 0, then commit. */
    l->base = base;
    l->bank_size = bank_size;
    if (!erase_bank(l, 0)) return NV_LOG_MIGRATION_ERROR;
    uint32_t gen = l->generation + 1u;
    if (!write_header(l, 0, gen, false)) return NV_LOG_MIGRATION_ERROR;
    uint32_t off = NV_LOG_HEADER;
    unsigned copied = 0;
    for (unsigned id = 0; id < NV_LOG_MAX_IDS; id++) {
        if (!l->scratch_index[id]) continue;
        uint16_t rid, len;
        uint32_t next;
        l->base = old_base;
        l->bank_size = old_bank_size;
        int r = read_record(l, old_active, (uint32_t)l->scratch_index[id] * 4u, &rid, &len, &next, l->tmp2);
        l->base = base;
        l->bank_size = bank_size;
        if (r != 1 || rid != id) continue;   /* unreadable: as nv_log_read would report it */
        uint32_t at = off;
        if (!append(l, 0, &off, (uint16_t)id, len, l->tmp2)) return NV_LOG_MIGRATION_ERROR;
        l->index[id] = (uint16_t)(at / 4u);
        copied++;
    }
    const uint8_t zero[4] = {0, 0, 0, 0};
    if (!program(l, bank_addr(l, 0) + 12u, zero, 4)) return NV_LOG_MIGRATION_ERROR;   /* commit point */
    /* 3. Mounted on the new copy. */
    l->active = 0;
    l->generation = gen;
    l->write_offset = off;
    l->full = false;
    l->mounted = true;
    l->mount_status = NV_LOG_MIGRATED;
    l->stats.mounts++;
    l->migrated_records = copied;
    /* 4. The old store is retired (its headers erased): an older firmware
     * would find no valid store rather than resume stale data. A power cut
     * here is finished by nv_log_retire_old() at the next boot. */
    (void)nv_log_retire_old(l, old_base, old_bank_size);
    return NV_LOG_MIGRATION_DONE;
}

bool nv_log_retire_old(nv_log_t *l, uint32_t old_base, uint32_t old_bank_size) {
    if (!l->mounted || !l->sector_size) return false;
    /* Only while the new bank that covers the old store has never been used
     * by the new layout (its header still erased): otherwise those sectors
     * hold new data, never touched here. */
    for (unsigned b = 0; b < 2; b++) {
        uint32_t nb = bank_addr(l, b);
        if (!ranges_overlap(nb, l->bank_size, old_base, 2u * old_bank_size)) continue;
        uint8_t h[16];
        if (!l->ops.read(l->ops.ctx, nb, h, sizeof h)) return false;
        for (unsigned i = 0; i < sizeof h; i++) if (h[i] != 0xFF) return false;
        if (b == l->active) return false;
    }
    bool retired = false;
    for (unsigned b = 0; b < 2; b++) {
        uint32_t ob = old_base + b * old_bank_size;
        uint8_t h[16];
        if (!l->ops.read(l->ops.ctx, ob, h, sizeof h)) return false;
        bool blank = true;
        for (unsigned i = 0; i < sizeof h; i++) blank &= h[i] == 0xFF;
        if (blank) continue;
        if (!l->ops.erase_sector(l->ops.ctx, ob - (ob % l->sector_size))) { l->stats.erase_failures++; return false; }
        retired = true;
    }
    if (retired) l->stats.old_layout_retired++;
    return retired;
}
