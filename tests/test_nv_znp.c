/* Tests the actual modified TI NVOCMP implementation against simulated NOR.
 * This is a persistence model test, not a test on physical Flash.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv_cache.h"
#include "nvocmp.c"
unsigned test_gate_depth;
static uint8_t flash_image[3][8192];
static unsigned reads, programs, erases;
static int cut_after = -1;
static bool initialized;
void NV_LINUX_init(void) { if (!initialized) { memset(flash_image, 255, sizeof(flash_image)); initialized = true; } }
void NV_LINUX_save(void) { }
void NV_LINUX_read(uint8_t p, uint16_t o, uint8_t *b, uint16_t n) {
    assert(p < 3 && (unsigned)o + n <= 8192); reads++; memcpy(b, flash_image[p] + o, n);
}
int NV_LINUX_write(uint8_t p, uint16_t o, uint8_t *b, uint16_t n) {
    assert(p < 3 && (unsigned)o + n <= 8192); programs++;
    for (unsigned i = 0; i < n; i++) {
        if (cut_after == 0) return -1;
        assert((flash_image[p][o + i] & b[i]) == b[i]); /* NOR permits 1->0 only. */
        flash_image[p][o + i] &= b[i];
        if (cut_after > 0) cut_after--;
    }
    return 0;
}
int NV_LINUX_erase(uint8_t p) {
    assert(p < 3); erases++;
    if (cut_after == 0) return -1;
    memset(flash_image[p], 255, 8192); return 0;
}
static NVINTF_nvFuncts_t api;
static void reboot(void) {
    NVOCMP_failF = NVINTF_NOTREADY; /* Model RAM loss in the real TI implementation. */
    test_gate_depth = 0;
    port_nv_cache_invalidate();
    NVOCMP_loadApiPtrsExt(&api);
    assert(api.initNV(NULL) == NVINTF_SUCCESS);
    assert(!test_gate_depth);
}
static void row_data(uint16_t item, unsigned sub, uint8_t *row) {
    unsigned length = item == 4 ? 20 : 12, offset = item == 4 ? 8 : 4;
    memset(row, 0, length);
    row[0] = (uint8_t)sub;
    if (item == 1) { unsigned nwk = sub < 74 ? 0x1000u + sub : 0xffff; row[2] = (uint8_t)nwk; row[3] = (uint8_t)(nwk >> 8); }
    if (sub < 74) { row[offset] = (uint8_t)(sub + 1); row[offset + 7] = 0xb5; }
}
static void large_directory_test(void) {
    uint8_t row[20], output[20];
    for (unsigned item = 1; item <= 4; item += 3) {
        unsigned length = item == 4 ? 20 : 12;
        for (unsigned sub = 0; sub < 200; sub++) {
            NVINTF_itemID_t id = {1, (uint16_t)item, (uint16_t)sub};
            row_data((uint16_t)item, sub, row);
            assert(api.writeItem(id, (uint16_t)length, row) == NVINTF_SUCCESS);
        }
    }
    NVINTF_itemID_t first = {1, 4, 0};
    assert(api.getItemLen(first) == 20);
    assert(port_nv_index_ready());
    unsigned start = reads;
    for (unsigned item = 1; item <= 4; item += 3) {
        unsigned length = item == 4 ? 20 : 12;
        for (unsigned sub = 0; sub < 200; sub++) {
            NVINTF_itemID_t id = {1, (uint16_t)item, (uint16_t)sub};
            unsigned previous = reads;
            assert(api.getItemLen(id) == length);
            assert(reads - previous == 1); /* One header read, no log traversal. */
        }
    }
    assert(reads - start == 400);
    for (unsigned round = 0; round < 2; round++) {
        for (unsigned item = 1; item <= 4; item += 3) {
            unsigned length = item == 4 ? 20 : 12;
            for (unsigned sub = 0; sub < 200; sub++) {
                NVINTF_itemID_t id = {1, (uint16_t)item, (uint16_t)sub};
                row_data((uint16_t)item, sub, row);
                unsigned previous = reads;
                assert(api.readItem(id, 0, (uint16_t)length, output) == NVINTF_SUCCESS);
                assert(!memcmp(row, output, length));
                assert(reads - previous <= 4);
            }
        }
    }
    uint16_t found;
    /* Three learned content patterns, in addition to the earlier NV test's
     * 64-byte pattern. Empty/default rows share query bytes and must return
     * the newest matching sub-ID, exactly as TI's reverse log search does. */
    for (unsigned shape = 0; shape < 3; shape++) {
        unsigned item = shape == 0 ? 4 : 1;
        unsigned length = item == 4 ? 20 : 12;
        unsigned offset = shape == 0 ? 8 : (shape == 1 ? 4 : 2);
        unsigned query_length = shape == 2 ? 2 : 8;
        NVINTF_itemID_t id = {1, (uint16_t)item, 0};
        row_data((uint16_t)item, 0, row);
        assert(api.readContItem(id, 0, (uint16_t)length, output, (uint16_t)query_length,
               (uint16_t)offset, row + offset, &found) == NVINTF_SUCCESS && found == 0);
        assert(port_nv_index_ready());
        for (unsigned round = 0; round < 3; round++) for (unsigned sub = 0; sub < 74; sub++) {
            row_data((uint16_t)item, sub, row);
            unsigned previous = reads;
            assert(api.readContItem(id, 0, (uint16_t)length, output, (uint16_t)query_length,
                   (uint16_t)offset, row + offset, &found) == NVINTF_SUCCESS && found == sub);
            assert(!memcmp(row, output, length) && reads - previous <= 6);
        }
        row_data((uint16_t)item, 199, row);
        assert(api.readContItem(id, 0, (uint16_t)length, output, (uint16_t)query_length,
               (uint16_t)offset, row + offset, &found) == NVINTF_SUCCESS && found == 199);
    }
    uint8_t missing[8];
    start = reads;
    for (unsigned i = 0; i < 1000; i++) {
        memset(missing, 0xff, sizeof(missing)); missing[0] = (uint8_t)i; missing[1] = (uint8_t)(i >> 8);
        assert(api.readContItem(first, 0, 20, output, 8, 8, missing, &found) == NVINTF_NOTFOUND);
    }
    assert(reads == start); /* 1000 different uncached negative queries. */
    /* Interleave a persisted counter update with hot key and EUI lookups
     * from another family. None should reread NOR after the successful commit.
     * The mutated ID itself must be reread and return its new bytes. */
    NVINTF_itemID_t hot = {1, 1, 0}, counter = {1, 4, 1};
    uint8_t hot_row[12]; row_data(1, 0, hot_row);
    assert(api.readItem(hot, 0, 12, output) == NVINTF_SUCCESS);
    assert(api.readContItem(hot, 0, 12, output, 8, 4, hot_row + 4, &found) == NVINTF_SUCCESS);
    memset(missing, 0xee, 8);
    assert(api.readContItem(hot, 0, 12, output, 8, 4, missing, &found) == NVINTF_NOTFOUND);
    assert(api.readItem(counter, 0, 20, output) == NVINTF_SUCCESS);
    row_data(4, 1, row); row[0] += 9;
    assert(api.writeItem(counter, 20, row) == NVINTF_SUCCESS && port_nv_index_ready());
    start = reads;
    assert(api.readItem(hot, 0, 12, output) == NVINTF_SUCCESS && !memcmp(output, hot_row, 12));
    assert(api.readContItem(hot, 0, 12, output, 8, 4, hot_row + 4, &found) == NVINTF_SUCCESS && found == 0);
    assert(api.readContItem(hot, 0, 12, output, 8, 4, missing, &found) == NVINTF_NOTFOUND);
    assert(reads == start);
    assert(api.readItem(counter, 0, 20, output) == NVINTF_SUCCESS && !memcmp(output, row, 20));
    assert(reads > start);
    unsigned builds = port_nv_index_stats.builds, erased = erases;
    for (unsigned sub = 0; sub < 40; sub++) {
        NVINTF_itemID_t id = {1, 4, (uint16_t)sub};
        row_data(4, sub, row); row[0] += 7; /* Counter changes, EUI64 unchanged. */
        assert(api.writeItem(id, 20, row) == NVINTF_SUCCESS);
        assert(port_nv_index_ready() && port_nv_index_stats.builds == builds && erases == erased);
        assert(api.readContItem(first, 0, 20, output, 8, 8, row + 8, &found) == NVINTF_SUCCESS && found == sub);
        assert(!memcmp(row, output, 20));
    }
    /* A counter-only update of a duplicate query must promote this record to
     * the newest match without rescanning the other 400 records. */
    NVINTF_itemID_t duplicate = {1, 4, 150};
    row_data(4, 150, row); row[0]++;
    assert(api.writeItem(duplicate, 20, row) == NVINTF_SUCCESS);
    assert(port_nv_index_ready() && port_nv_index_stats.builds == builds);
    assert(api.readContItem(first, 0, 20, output, 8, 8, row + 8, &found) == NVINTF_SUCCESS && found == 150);
    /* A membership change rebuilds before answering either the old or new
     * query; the old positive result must not survive in any RAM structure. */
    uint8_t old_query[8]; row_data(4, 0, row); memcpy(old_query, row + 8, 8);
    row[8] = 0x91; row[15] = 0x66;
    assert(api.writeItem(first, 20, row) == NVINTF_SUCCESS && !port_nv_index_ready());
    assert(api.readContItem(first, 0, 20, output, 8, 8, old_query, &found) == NVINTF_NOTFOUND);
    assert(port_nv_index_ready() && port_nv_index_stats.builds == builds + 1);
    assert(api.readContItem(first, 0, 20, output, 8, 8, row + 8, &found) == NVINTF_SUCCESS && found == 0);
    assert(api.deleteItem(duplicate) == NVINTF_SUCCESS);
    memset(old_query, 0, sizeof(old_query));
    assert(api.readContItem(first, 0, 20, output, 8, 8, old_query, &found) == NVINTF_SUCCESS && found == 199);
    /* Silent NOR data damage after directory construction: a CRC failure in
     * the newest duplicate must fall back to the next valid native match. */
    for (unsigned i = 0; i < 8; i++) {
        NVINTF_itemID_t evict = {1, 1, (uint16_t)(100 + i)};
        assert(api.readItem(evict, 0, 12, output) == NVINTF_SUCCESS);
    }
    uint16_t locations[PORT_NV_INDEX_CANDIDATES];
    assert(port_nv_index_get(NVOCMP_CMPRID(1, 4, 199), locations) == 1);
    NVOCMP_itemHdr_t corrupt;
    assert(port_nv_index_header(locations[0], &corrupt));
    flash_image[corrupt.hpage][corrupt.hofs - corrupt.len] &= 0xfe;
    assert(api.readContItem(first, 0, 20, output, 8, 8, old_query, &found) == NVINTF_SUCCESS && found == 198);
    NVINTF_itemID_t target = {1, 4, 1};
    uint8_t saved_flash[sizeof(flash_image)], previous[20], next[20];
    assert(api.readItem(target, 0, 20, previous) == NVINTF_SUCCESS);
    memcpy(next, previous, 20); next[0] ^= 0x55;
    memcpy(saved_flash, flash_image, sizeof(saved_flash));
    for (int cut = 0; cut < 40; cut++) {
        memcpy(flash_image, saved_flash, sizeof(saved_flash)); reboot();
        assert(api.readItem(target, 0, 20, output) == NVINTF_SUCCESS);
        assert(api.readItem(hot, 0, 12, output) == NVINTF_SUCCESS);
        unsigned invalidations = port_nv_stats.invalidations;
        cut_after = cut; uint8_t written = api.writeItem(target, 20, next); cut_after = -1;
        if (written != NVINTF_SUCCESS && port_nv_stats.invalidations != invalidations)
            assert(!port_nv_cache_get(1, 1, 0, 0, 12, output));
        reboot();
        assert(api.readItem(target, 0, 20, output) == NVINTF_SUCCESS);
        assert(!memcmp(output, previous, 20) || !memcmp(output, next, 20));
        uint8_t persisted[20]; memcpy(persisted, output, 20);
        assert(api.readContItem(first, 0, 20, output, 8, 8, previous + 8, &found) == NVINTF_SUCCESS && found == 1);
        assert(!memcmp(output, persisted, 20) && !test_gate_depth);
    }
    assert(api.createItem(target, 65536u + 20u, next) == NVINTF_BADLENGTH);
    printf("PASS: actual NV directory over 400 table records/74 populated devices, 400 ID lookups at one NOR read each, cold content reads bounded, newest duplicate default, 1000 distinct absent EUI64 queries/zero NOR reads\n");
    puts("PASS: NV index counter updates without rebuild, duplicate promotion, CRC-damaged newest duplicate falls back to older valid record, membership/delete invalidation, 40 indexed torn-write recovery points, oversized create rejected before truncation");
    puts("PASS: unrelated hot ID/content/negative cache entries survive a verified counter commit with zero NOR reads; changed ID and failed writes never revive old data");
}
int main(void) {
    reboot();
    NVINTF_itemID_t id = {1, 42, 7}, other = {1, 42, 8};
    uint8_t data[64], out[80], changed[64];
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    memset(changed, 0x5a, sizeof(changed));
    assert(api.writeItem(id, sizeof(data), data) == NVINTF_SUCCESS);
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS);
    unsigned before = reads;
    for (unsigned i = 0; i < 1000; i++) {
        memset(out, 0, sizeof(out));
        assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS);
        assert(memcmp(data, out, sizeof(data)) == 0);
    }
    assert(reads == before); /* Actual NVOCMP API hits perform ZERO NOR reads. */
    /* Exact full generation cycle: a stale tag would match without clearing. */
    for (unsigned i = 0; i < 65536; i++) port_nv_cache_invalidate();
    before = reads;
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS && reads > before);
    uint16_t found;
    assert(api.readContItem(id, 0, sizeof(data), out, 8, 0, data, &found) == NVINTF_SUCCESS && found == id.subID);
    before = reads;
    for (unsigned i = 0; i < 1000; i++) {
        assert(api.readContItem(id, 0, sizeof(data), out, 8, 0, data, &found) == NVINTF_SUCCESS);
        assert(found == id.subID && !memcmp(out, data, sizeof(data)));
    }
    assert(reads == before);
    uint8_t missing[8]; memset(missing, 0xfa, sizeof(missing));
    assert(api.readContItem(id, 0, sizeof(data), out, 8, 60, data, &found) == NVINTF_BADPARAM);
    assert(api.readContItem(id, 0, sizeof(data), out, 8, 0, data, NULL) == NVINTF_BADPARAM);
    assert(api.readContItem(id, 0, sizeof(data), out, 8, 0, missing, &found) == NVINTF_NOTFOUND && found == 0xffff);
    before = reads;
    for (unsigned i = 0; i < 1000; i++) {
        assert(api.readContItem(id, 0, sizeof(data), out, 8, 0, missing, &found) == NVINTF_NOTFOUND && found == 0xffff);
    }
    assert(reads == before);
    uint8_t new_record[64]; memcpy(new_record, data, sizeof(data)); memcpy(new_record, missing, sizeof(missing));
    assert(api.writeItem(other, sizeof(new_record), new_record) == NVINTF_SUCCESS);
    assert(api.readContItem(id, 0, sizeof(data), out, 8, 0, missing, &found) == NVINTF_SUCCESS && found == other.subID);
    assert(api.deleteItem(other) == NVINTF_SUCCESS);
    assert(api.readItem(id, 1, 16, out) == NVINTF_SUCCESS && !memcmp(out, data + 1, 16));
    before = programs;
    unsigned erased_before = erases;
    for (unsigned i = 0; i < 1000; i++) assert(api.writeItem(id, sizeof(data), data) == NVINTF_SUCCESS);
    assert(programs == before && erases == erased_before);
    assert(api.writeItem(id, sizeof(changed), changed) == NVINTF_SUCCESS);
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS && !memcmp(out, changed, sizeof(data)));
    assert(api.readItem(id, 1, 16, out) == NVINTF_SUCCESS && !memcmp(out, changed + 1, 16));
    assert(api.readItem(id, 63, 2, out) == NVINTF_BADOFFSET);
    assert(api.readItem(id, UINT16_MAX, 2, out) == NVINTF_BADOFFSET);
    assert(api.readItem(id, 0, sizeof(out), out) == NVINTF_BADLENGTH);
    assert(api.readItem(id, 0, 0, out) == NVINTF_BADPARAM);
    assert(api.readItem(id, 0, 1, NULL) == NVINTF_BADPARAM);
    assert(api.readItem(other, 0, 1, out) == NVINTF_NOTFOUND);
    assert(api.deleteItem(id) == NVINTF_SUCCESS);
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_NOTFOUND);
    assert(api.createItem(id, sizeof(data), data) == NVINTF_SUCCESS);
    assert(api.createItem(id, sizeof(data), data) == NVINTF_EXIST);
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS && !memcmp(out, data, sizeof(data)));
    /* A prefix cached from a larger item must not suppress a length-changing write. */
    assert(api.writeItem(id, 16, changed) == NVINTF_SUCCESS);
    assert(api.getItemLen(id) == 16);
    assert(api.writeItem(id, sizeof(data), data) == NVINTF_SUCCESS);
    for (unsigned i = 0; i < 1200; i++) {
        data[0] = (uint8_t)i;
        assert(api.writeItem(id, sizeof(data), data) == NVINTF_SUCCESS);
    }
    assert(erases > erased_before);
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS && !memcmp(out, data, sizeof(data)));
    before = programs; erased_before = erases;
    assert(api.writeItem(id, sizeof(data), data) == NVINTF_SUCCESS);
    assert(programs == before && erases == erased_before);
    assert(api.compactNV(0) == NVINTF_SUCCESS);
    before = reads;
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS && !memcmp(out, data, sizeof(data)));
    assert(reads > before); /* Compaction never resumes old cache tags. */
    reboot();
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS && !memcmp(out, data, sizeof(data)));
    /* Tear a write at every byte boundary, then reboot through TI recovery. */
    uint8_t saved[sizeof(flash_image)];
    memcpy(saved, flash_image, sizeof(saved));
    for (int cut = 0; cut < 80; cut++) {
        memcpy(flash_image, saved, sizeof(saved)); reboot();
        assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS);
        cut_after = cut;
        (void)api.writeItem(id, sizeof(changed), changed);
        cut_after = -1;
        reboot();
        assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_SUCCESS);
        assert(!memcmp(out, data, sizeof(data)) || !memcmp(out, changed, sizeof(changed)));
        assert(!test_gate_depth);
    }
    large_directory_test();
    assert(api.eraseNV() == NVINTF_SUCCESS);
    assert(api.readItem(id, 0, sizeof(data), out) == NVINTF_NOTFOUND);
    printf("PASS: actual TI NVOCMP, 1000 ID + 1000 content + 1000 negative cache hits/zero NOR reads, 1000 identical writes/zero NOR programs, resize/delete/compaction/reboot, 80 torn-write recovery points\n");
    return 0;
}
