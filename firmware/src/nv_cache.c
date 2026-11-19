/* SPDX-License-Identifier: MIT */
#include "nv_cache.h"
#include "nv_index.h"
#include <string.h>
typedef struct {
    uint16_t epoch;
    uint16_t item, sub, offset, length, found_sub;
    /* Content offset/length are bounded to 64/8 before insertion. */
    uint8_t coff, clen, system, content, status, query[8], bytes[PORT_NV_CACHE_BYTES];
} cache_slot;
_Static_assert(sizeof(cache_slot) <= 90, "NV cache metadata exceeds the CC2652P RAM budget");
static cache_slot slots[PORT_NV_CACHE_SLOTS];
static uint16_t epoch = 1;
static uint8_t replacement;
port_nv_stats_t port_nv_stats;
_Static_assert((PORT_NV_CACHE_SLOTS & (PORT_NV_CACHE_SLOTS - 1)) == 0, "Power of two required");
static cache_slot *replacement_slot(void) {
    for (unsigned i = 0; i < PORT_NV_CACHE_SLOTS; i++) if (slots[i].epoch != epoch) return &slots[i];
    cache_slot *s = &slots[replacement];
    replacement = (replacement + 1u) & (PORT_NV_CACHE_SLOTS - 1u);
    return s;
}
static cache_slot *id_slot(uint8_t system, uint16_t item, uint16_t sub, uint16_t offset, uint16_t length) {
    for (unsigned i = 0; i < PORT_NV_CACHE_SLOTS; i++) {
        cache_slot *s = &slots[i];
        if (s->epoch == epoch && !s->content && s->system == system && s->item == item && s->sub == sub &&
            s->offset == offset && s->length == length) return s;
    }
    return NULL;
}
bool port_nv_cache_get(uint8_t system, uint16_t item, uint16_t sub,
                       uint16_t offset, uint16_t length, void *buffer) {
    if (!length || length > PORT_NV_CACHE_BYTES || !buffer) { port_nv_stats.bypasses++; return false; }
    cache_slot *s = id_slot(system, item, sub, offset, length);
    if (!s) { port_nv_stats.misses++; return false; }
    memcpy(buffer, s->bytes, length);
    port_nv_stats.hits++;
    return true;
}
void port_nv_cache_put(uint8_t system, uint16_t item, uint16_t sub,
                       uint16_t offset, uint16_t length, const void *buffer) {
    if (!length || length > PORT_NV_CACHE_BYTES || !buffer) return;
    cache_slot *s = id_slot(system, item, sub, offset, length);
    if (!s) s = replacement_slot();
    s->system = system; s->item = item; s->sub = sub; s->offset = offset; s->length = length;
    s->content = 0;
    memcpy(s->bytes, buffer, length);
    s->epoch = epoch;
}
static bool content_valid(uint16_t length, uint16_t coff, uint16_t clen, const void *query) {
    return length && length <= PORT_NV_CACHE_BYTES && clen && clen <= 8 && query && (uint32_t)coff + clen <= length;
}
static cache_slot *content_slot(uint8_t system, uint16_t item, uint16_t sub, uint16_t offset,
    uint16_t length, uint16_t coff, uint16_t clen, const void *query) {
    for (unsigned i = 0; i < PORT_NV_CACHE_SLOTS; i++) {
        cache_slot *s = &slots[i];
        if (s->epoch == epoch && s->content && s->system == system && s->item == item && s->sub == sub &&
            s->offset == offset && s->length == length && s->coff == coff && s->clen == clen && !memcmp(s->query, query, clen)) return s;
    }
    return NULL;
}
bool port_nv_content_get(uint8_t system, uint16_t item, uint16_t sub, uint16_t offset,
    uint16_t length, uint16_t coff, uint16_t clen, const void *query, void *buffer,
    uint16_t *found_sub, uint8_t *status) {
    if (!buffer || !found_sub || !status || !content_valid(length, coff, clen, query)) return false;
    cache_slot *s = content_slot(system, item, sub, offset, length, coff, clen, query);
    if (!s) return false;
    if (!s->status) memcpy(buffer, s->bytes, length);
    *found_sub = s->found_sub; *status = s->status;
    port_nv_stats.content_hits++;
    return true;
}
void port_nv_content_put(uint8_t system, uint16_t item, uint16_t sub, uint16_t offset,
    uint16_t length, uint16_t coff, uint16_t clen, const void *query, const void *buffer,
    uint16_t found_sub, uint8_t status) {
    if (!buffer || (status != 0 && status != 10) || !content_valid(length, coff, clen, query)) return;
    cache_slot *s = content_slot(system, item, sub, offset, length, coff, clen, query);
    if (!s) s = replacement_slot();
    s->system = system; s->item = item; s->sub = sub; s->offset = offset; s->length = length;
    s->content = 1; s->coff = coff; s->clen = clen; s->status = status; s->found_sub = found_sub;
    memcpy(s->query, query, clen);
    if (!status) memcpy(s->bytes, buffer, length);
    s->epoch = epoch;
}
void port_nv_cache_invalidate(void) {
    port_nv_index_invalidate();
    port_nv_stats.invalidations++;
    if (++epoch == 0) {
        /* Exception after 2^16 physical mutations; normal invalidation is O(1). */
        memset(slots, 0, sizeof(slots));
        epoch = 1;
    }
}
port_nv_cache_checkpoint port_nv_cache_checkpoint_get(void) {
    return (port_nv_cache_checkpoint){port_nv_stats.invalidations, epoch};
}
void port_nv_cache_resume(const port_nv_cache_checkpoint *checkpoint,
                          uint8_t system, uint16_t item, uint16_t sub) {
    uint32_t mutations = port_nv_stats.invalidations - checkpoint->invalidations;
    /* A complete epoch cycle clears every slot. Never revive its old tags. */
    if (!mutations || mutations >= UINT16_MAX) return;
    for (unsigned i = 0; i < PORT_NV_CACHE_SLOTS; i++) {
        cache_slot *s = &slots[i];
        if (s->epoch != checkpoint->epoch) continue;
        if (s->system == system && s->item == item && (s->content || s->sub == sub)) continue;
        s->epoch = epoch;
    }
}
