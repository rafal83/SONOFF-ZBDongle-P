/* SPDX-License-Identifier: MIT
 * Included after the TI NVOCMP definitions; no persistent format changes.
 */
typedef struct { uint16_t id; uint8_t length, offset, query_length; } port_nv_pattern;
static port_nv_pattern port_nv_patterns[4];
static uint8_t port_nv_pattern_count;
static bool port_nv_index_building;
static uint8_t port_nv_write_depth;
static uint32_t port_nv_content_key(port_nv_pattern p, const uint8_t *query) {
    uint32_t h = 2166136261u ^ p.id ^ ((uint32_t)p.length << 16) ^
                 ((uint32_t)p.offset << 8) ^ p.query_length;
    for (unsigned i = 0; i < p.query_length; i++) h = (h ^ query[i]) * 16777619u;
    return h | 0x80000000u;
}
static bool port_nv_index_header(uint16_t location, NVOCMP_itemHdr_t *header) {
    NVOCMP_readHeader(location >> 13, location & 8191u, header, false);
    return (header->stats & (NVOCMP_ACTIVEIDBIT | NVOCMP_VALIDIDBIT | NVOCMP_FOLLOWBIT)) ==
           (NVOCMP_ACTIVEIDBIT | NVOCMP_FOLLOWBIT) &&
           header->len <= header->hofs - NVOCMP_PGDATAOFS;
}
static bool port_nv_pattern_equal(port_nv_pattern p, NVOCMP_itemHdr_t h, NVOCMP_itemInfo_t *info) {
    return p.id == ((h.sysid << 10) | h.itemid) && p.length == info->rlength &&
           p.offset == info->coff && p.query_length == info->clength;
}
static bool port_nv_index_build(NVOCMP_nvHandle_t *handle) {
    uint32_t generation = port_nv_index_stats.generation;
    uint32_t previous = 0;
    uint8_t page = handle->actPage;
    uint16_t offset = handle->actOffset;
    uint8_t buffer[64], older[64];
    uint16_t locations[PORT_NV_INDEX_CANDIDATES];
    bool complete = false;
    port_nv_index_building = true;
    port_nv_index_begin();
    for (unsigned scanned = 0; scanned < 4096; scanned++) {
        NVOCMP_itemHdr_t h = {0};
        int8_t status = NVOCMP_findItem(handle, page, offset, &h, NVOCMP_FINDANY, NULL);
        if (port_nv_index_stats.generation != generation) break;
        if (status == NVINTF_NOTFOUND) { complete = true; break; }
        if (status != NVINTF_SUCCESS) break;
        uint32_t order = ((handle->actPage + NVOCMP_NVSIZE - h.hpage) % NVOCMP_NVSIZE) * FLASH_PAGE_SIZE +
                         FLASH_PAGE_SIZE - h.hofs;
        /* Native findItem searches a circular log; stop before wrapping back
         * to a record already visited, even when older active pages exist. */
        if (order <= previous) { complete = true; break; }
        previous = order;
        if (h.hpage >= 3 || h.hofs < NVOCMP_PGDATAOFS || h.hofs >= FLASH_PAGE_SIZE ||
            h.len > h.hofs - NVOCMP_PGDATAOFS) break;
        uint16_t location = ((uint16_t)h.hpage << 13) | h.hofs;
        if (!port_nv_index_get(h.cmpid, locations) && !port_nv_index_put(h.cmpid, location, false)) break;
        for (unsigned pattern = 0; pattern < port_nv_pattern_count; pattern++) {
            port_nv_pattern p = port_nv_patterns[pattern];
            if (p.id != ((h.sysid << 10) | h.itemid) || p.length > h.len) continue;
            if (NVOCMP_readItem(&h, 0, p.length, buffer, false) != NVINTF_SUCCESS) continue;
            uint32_t key = port_nv_content_key(p, buffer + p.offset);
            unsigned candidates = port_nv_index_get(key, locations);
            bool duplicate = false;
            for (unsigned i = 0; i < candidates; i++) {
                NVOCMP_itemHdr_t candidate;
                if (!port_nv_index_header(locations[i], &candidate)) continue;
                if (candidate.sysid != h.sysid || candidate.itemid != h.itemid || candidate.len < p.length) continue;
                if (NVOCMP_readItem(&candidate, 0, p.length, older, false) == NVINTF_SUCCESS &&
                    !memcmp(older + p.offset, buffer + p.offset, p.query_length)) { duplicate = true; break; }
            }
            if (!duplicate && !port_nv_index_put(key, location, false)) break;
        }
        if (port_nv_index_blocked()) break;
        page = h.hpage; offset = h.hofs - h.len;
    }
    port_nv_index_building = false;
    if (port_nv_index_stats.generation != generation) return false;
    port_nv_index_finish(complete);
    return port_nv_index_ready();
}
static bool port_nv_index_find(NVOCMP_nvHandle_t *handle, uint8_t page, uint16_t offset,
    NVOCMP_itemHdr_t *header, int8_t flag, NVOCMP_itemInfo_t *info, int8_t *result) {
    if (port_nv_index_building || NVOCMP_failF != NVINTF_SUCCESS || handle != &NVOCMP_nvHandle ||
        page != handle->actPage || offset != handle->actOffset) return false;
    if (port_nv_write_depth && !port_nv_index_ready()) return false;
    bool content = flag == (NVOCMP_FINDITMID | NVOCMP_FINDCONTENT) && info;
    uint32_t key = NVOCMP_CMPRID(header->sysid, header->itemid, header->subid);
    if (content) {
        if (!info->cBuf || !info->rBuf || !info->clength || info->clength > 8 || !info->rlength ||
            info->rlength > 64 || (uint32_t)info->coff + info->clength > info->rlength) return false;
        unsigned i;
        for (i = 0; i < port_nv_pattern_count; i++)
            if (port_nv_pattern_equal(port_nv_patterns[i], *header, info)) break;
        if (i == port_nv_pattern_count) {
            if (i == 4) return false;
            port_nv_patterns[i] = (port_nv_pattern){(uint16_t)((header->sysid << 10) | header->itemid),
                (uint8_t)info->rlength, (uint8_t)info->coff, (uint8_t)info->clength};
            port_nv_pattern_count++;
            port_nv_index_invalidate();
        }
        key = port_nv_content_key(port_nv_patterns[i], info->cBuf);
    } else if (flag != NVOCMP_FINDSTRICT || info) return false;
    if (!port_nv_index_ready() && (port_nv_index_blocked() || !port_nv_index_build(handle))) return false;
    uint16_t locations[PORT_NV_INDEX_CANDIDATES];
    unsigned count = port_nv_index_get(key, locations);
    uint32_t best_order = UINT32_MAX;
    NVOCMP_itemHdr_t best;
    for (unsigned i = 0; i < count; i++) {
        NVOCMP_itemHdr_t candidate;
        if (!port_nv_index_header(locations[i], &candidate)) { port_nv_index_invalidate(); return false; }
        if (!content) {
            if (candidate.cmpid != key) { port_nv_index_invalidate(); return false; }
            *header = candidate; *result = NVINTF_SUCCESS; return true;
        }
        if (candidate.sysid != header->sysid || candidate.itemid != header->itemid) continue;
        if (NVOCMP_readItem(&candidate, 0, info->rlength, info->rBuf, false) != NVINTF_SUCCESS) {
            /* Only the newest equal-content record is indexed. If it became
             * corrupt, the native search must be allowed to find an older
             * valid duplicate; never manufacture NOTFOUND from a CRC error. */
            port_nv_index_invalidate(); return false;
        }
        if (!memcmp((uint8_t *)info->rBuf + info->coff, info->cBuf, info->clength)) {
            uint32_t order = ((handle->actPage + NVOCMP_NVSIZE - candidate.hpage) % NVOCMP_NVSIZE) * FLASH_PAGE_SIZE +
                             FLASH_PAGE_SIZE - candidate.hofs;
            if (order < best_order) { best_order = order; best = candidate; }
        }
    }
    if (content && best_order != UINT32_MAX) {
        *result = NVOCMP_readItem(&best, 0, info->rlength, info->rBuf, false);
        *header = best;
        return true;
    }
    header->hofs = 0; *result = NVINTF_NOTFOUND; return true;
}
static void port_nv_transaction_begin(port_nv_transaction *state, NVINTF_itemID_t id) {
    *state = (port_nv_transaction){0};
    state->cache_checkpoint = port_nv_cache_checkpoint_get();
    state->generation = port_nv_index_stats.generation;
    state->barriers = port_nv_index_stats.barriers;
    state->patterns = port_nv_pattern_count;
    state->restore = port_nv_index_ready() && !port_nv_write_depth;
    if (port_nv_write_depth) port_nv_index_barrier();
    port_nv_write_depth++;
    if (!state->restore) return;
    uint32_t key = NVOCMP_CMPRID(id.systemID, id.itemID, id.subID);
    uint16_t locations[PORT_NV_INDEX_CANDIDATES];
    unsigned count = port_nv_index_get(key, locations);
    if (!count) return;
    if (!port_nv_index_header(locations[0], &state->previous) || state->previous.cmpid != key) {
        state->restore = false; port_nv_index_invalidate(); return;
    }
    state->had_previous = true;
    unsigned prefix = 0;
    for (unsigned i = 0; i < port_nv_pattern_count; i++) {
        port_nv_pattern p = port_nv_patterns[i];
        if (p.id == ((id.systemID << 10) | id.itemID) && p.length <= state->previous.len && p.length > prefix) prefix = p.length;
    }
    if (prefix && NVOCMP_readItem(&state->previous, 0, prefix, state->data, false) != NVINTF_SUCCESS) state->restore = false;
}
static void port_nv_transaction_end(port_nv_transaction *state, NVINTF_itemID_t id,
    uint32_t length, const void *data, uint8_t status) {
    port_nv_write_depth--;
    if (!state->restore || status != NVINTF_SUCCESS || state->generation == port_nv_index_stats.generation ||
        state->barriers != port_nv_index_stats.barriers || state->patterns != port_nv_pattern_count ||
        NVOCMP_failW != NVINTF_SUCCESS) return;
    NVOCMP_itemHdr_t current;
    NVOCMP_nvHandle_t *handle = &NVOCMP_nvHandle;
    if (handle->actOffset < NVOCMP_PGDATAOFS + NVOCMP_ITEMHDRLEN) return;
    uint16_t location = ((uint16_t)handle->actPage << 13) | (handle->actOffset - NVOCMP_ITEMHDRLEN);
    uint32_t key = NVOCMP_CMPRID(id.systemID, id.itemID, id.subID);
    if (!port_nv_index_header(location, &current) || current.cmpid != key || current.len != length) return;
    /* A changed query field or record membership needs a complete rebuild to
     * select the next-most-recent duplicate. Counter-only updates do not. */
    for (unsigned i = 0; i < port_nv_pattern_count; i++) {
        port_nv_pattern p = port_nv_patterns[i];
        if (p.id != ((id.systemID << 10) | id.itemID)) continue;
        bool was = state->had_previous && p.length <= state->previous.len;
        bool now = p.length <= length;
        if (was != now || (was && memcmp(state->data + p.offset, (const uint8_t *)data + p.offset, p.query_length))) return;
    }
    if (!port_nv_index_put(key, location, true)) return;
    uint16_t previous = ((uint16_t)state->previous.hpage << 13) | state->previous.hofs;
    for (unsigned i = 0; i < port_nv_pattern_count; i++) {
        port_nv_pattern p = port_nv_patterns[i];
        if (p.id != ((id.systemID << 10) | id.itemID) || p.length > length) continue;
        const uint8_t *query = (const uint8_t *)data + p.offset;
        uint32_t content_key = port_nv_content_key(p, query);
        uint16_t locations[PORT_NV_INDEX_CANDIDATES];
        unsigned candidates = port_nv_index_get(content_key, locations);
        for (unsigned j = 0; j < candidates; j++) {
            if (state->had_previous && locations[j] == previous) {
                port_nv_index_remove(content_key, locations[j]); continue;
            }
            NVOCMP_itemHdr_t h;
            uint8_t bytes[8];
            if (!port_nv_index_header(locations[j], &h)) return;
            if (h.sysid != id.systemID || h.itemid != id.itemID || h.len < p.length) continue;
            if (NVOCMP_readItem(&h, p.offset, p.query_length, bytes, false) == NVINTF_SUCCESS &&
                !memcmp(bytes, query, p.query_length)) port_nv_index_remove(content_key, locations[j]);
        }
        if (!port_nv_index_put(content_key, location, false)) return;
    }
    port_nv_index_finish(true);
    port_nv_cache_resume(&state->cache_checkpoint, id.systemID, id.itemID, id.subID);
}
