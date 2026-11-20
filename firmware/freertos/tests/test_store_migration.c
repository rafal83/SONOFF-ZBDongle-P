/* SPDX-License-Identifier: MIT
 * Storage layout 1 -> 2 migration of the nv_log store (nv_log_migrate /
 * nv_log_retire_old), driven exactly as radio_task does at boot, on a NOR
 * model of 0x34000-0x4FFFF with power cuts (torn programs, interrupted
 * erases) at EVERY flash operation of the boot sequence.
 *   layout 1: 2 banks x 16 KiB at 0x48000 (inside the new bank 1)
 *   layout 2: 2 banks x 56 KiB at 0x34000
 */
#include "frt_test.h"
#include "nv_log.h"

#define SECTOR 0x2000u
#define AREA_BASE 0x34000u
#define AREA_SIZE 0x1C000u
#define OLD_BASE 0x48000u
#define OLD_BANK 0x4000u
#define NEW_BASE 0x34000u
#define NEW_BANK 0xE000u

static uint8_t nor[AREA_SIZE], programmed[AREA_SIZE];
static long budget = -1;
static bool dead;
static unsigned double_programs, ops;
static uint32_t lcg = 777u;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static bool tick(void) {
    if (dead) return false;
    ops++;
    if (budget < 0) return true;
    if (budget-- == 0) { dead = true; return false; }
    return true;
}
static bool f_read(void *c, uint32_t a, void *b, size_t n) {
    (void)c;
    if (dead) return false;
    CHECK(a >= AREA_BASE && a + n <= AREA_BASE + AREA_SIZE);
    memcpy(b, nor + (a - AREA_BASE), n);
    return true;
}
static bool f_program(void *c, uint32_t a, const void *d, size_t n) {
    (void)c;
    CHECK(a >= AREA_BASE && a + n <= AREA_BASE + AREA_SIZE);
    bool ok = tick();
    size_t limit = ok ? n : (dead ? rnd() % (n + 1) : 0);
    for (size_t i = 0; i < limit; i++) {
        size_t o = a - AREA_BASE + i;
        if (programmed[o]) double_programs++;
        uint8_t v = ((const uint8_t *)d)[i];
        if (!ok && i + 1 == limit) v |= (uint8_t)rnd();
        nor[o] &= v;
        programmed[o] = 1;
    }
    return ok;
}
static bool f_erase(void *c, uint32_t a) {
    (void)c;
    CHECK(a >= AREA_BASE && (a - AREA_BASE) % SECTOR == 0);
    bool ok = tick();
    size_t limit = ok ? SECTOR : rnd() % SECTOR;
    for (size_t i = 0; i < limit; i++) { nor[a - AREA_BASE + i] = 0xFF; programmed[a - AREA_BASE + i] = 0; }
    return ok;
}
static const nv_flash_ops_t OPS = {f_read, f_program, f_erase, 0};

#define IDS 160u
static uint8_t model[IDS][48];
static uint16_t model_len[IDS];
static bool model_present[IDS];

static void check_model(nv_log_t *l) {
    for (unsigned id = 0; id < IDS; id++) {
        uint8_t b[NV_LOG_MAX_RECORD];
        uint16_t len = 0;
        bool present = nv_log_read(l, (uint16_t)id, b, sizeof b, &len);
        CHECK(present == model_present[id]);
        if (present) CHECK(len == model_len[id] && !memcmp(b, model[id], len));
    }
}

/* Read only: would a layout-1 firmware find a valid committed bank? (A real
 * mount would FORMAT a blank old area, i.e. write into the new bank 1: what
 * a firmware downgrade does, refused by the flashing tools.) */
static bool old_layout_valid(void) {
    for (unsigned b = 0; b < 2; b++) {
        const uint8_t *h = nor + (OLD_BASE + b * OLD_BANK - AREA_BASE);
        uint32_t magic = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
        uint32_t complete = (uint32_t)h[12] | (uint32_t)h[13] << 8 | (uint32_t)h[14] << 16 | (uint32_t)h[15] << 24;
        if (magic == 0x564E5246u && h[4] == 1 && h[5] == 0 && complete == 0) return true;
    }
    return false;
}

/* What radio_task does at boot. */
static bool boot(nv_log_t *l) {
    if (nv_log_mount(l, &OPS, NEW_BASE, NEW_BANK, SECTOR)) {
        (void)nv_log_retire_old(l, OLD_BASE, OLD_BANK);
        return true;
    }
    if (l->mount_status != NV_LOG_ERR_CORRUPT) return false;
    return nv_log_migrate(l, &OPS, OLD_BASE, OLD_BANK, NEW_BASE, NEW_BANK, SECTOR) == NV_LOG_MIGRATION_DONE;
}

static void write_rec(nv_log_t *l, unsigned id) {
    uint16_t len = (uint16_t)(4u + rnd() % 44u);
    uint8_t v[48];
    for (unsigned k = 0; k < len; k++) v[k] = (uint8_t)rnd();
    CHECK(nv_log_write(l, (uint16_t)id, v, len));
    model_present[id] = true;
    model_len[id] = len;
    memcpy(model[id], v, len);
}

int main(void) {
    static nv_log_t lg;
    static uint8_t snap_nor[AREA_SIZE], snap_prog[AREA_SIZE], snap_nor2[AREA_SIZE];

    /* 0. A blank device boots on the new layout directly (formatted). */
    memset(nor, 0xFF, sizeof nor);
    CHECK(boot(&lg) && lg.mount_status == NV_LOG_FORMATTED && lg.base == NEW_BASE);

    /* 1. A layout-1 store as in production: 150 records, rewrites,
     *    deletions, several compactions in its 16 KiB banks. */
    memset(nor, 0xFF, sizeof nor);
    memset(programmed, 0, sizeof programmed);
    CHECK(nv_log_mount(&lg, &OPS, OLD_BASE, OLD_BANK, SECTOR) && lg.mount_status == NV_LOG_FORMATTED);
    for (unsigned id = 0; id < 150; id++) write_rec(&lg, id);
    for (unsigned r = 0; r < 2500; r++) {
        unsigned id = rnd() % 150u;
        if (r % 23 == 0 && model_present[id]) { CHECK(nv_log_delete(&lg, (uint16_t)id)); model_present[id] = false; }
        else write_rec(&lg, id);
    }
    CHECK(lg.stats.compactions >= 3);
    check_model(&lg);
    memcpy(snap_nor, nor, sizeof nor);
    memcpy(snap_prog, programmed, sizeof programmed);

    /* 2. The new firmware's first boot: the new geometry is NOT formatted
     *    over the old data (corrupt -> migration), every record copied. */
    CHECK(!nv_log_mount(&lg, &OPS, NEW_BASE, NEW_BANK, SECTOR) && lg.mount_status == NV_LOG_ERR_CORRUPT);
    CHECK(!memcmp(nor, snap_nor, sizeof nor));                      /* nothing written by that attempt */
    unsigned ops0 = ops;
    CHECK(boot(&lg) && lg.mount_status == NV_LOG_MIGRATED && lg.stats.old_layout_retired == 1);
    unsigned boot_ops = ops - ops0;
    check_model(&lg);
    unsigned migrated = lg.migrated_records;
    /* An older firmware now finds no valid store (and would format nothing:
     * the rest of its banks still holds data, i.e. "corrupt"). */
    static nv_log_t old_view;
    CHECK(!old_layout_valid());
    memcpy(snap_nor2, nor, sizeof nor);
    CHECK(!nv_log_mount(&old_view, &OPS, OLD_BASE, OLD_BANK, SECTOR) && old_view.mount_status == NV_LOG_ERR_CORRUPT);
    CHECK(!memcmp(snap_nor2, nor, sizeof nor));
    /* Next boots: mounted normally, nothing more to retire. */
    CHECK(boot(&lg) && lg.mount_status == NV_LOG_MOUNTED && lg.stats.old_layout_retired == 0);
    check_model(&lg);
    printf("store_migration: %u records migrated in %u flash operations\n", migrated, boot_ops);

    /* 3. Power cut at EVERY flash operation of the migration boot, then a
     *    normal boot: never a record lost, the old store never left valid. */
    unsigned cuts = 0;
    for (unsigned k = 0; k < boot_ops; k++) {
        memcpy(nor, snap_nor, sizeof nor);
        memcpy(programmed, snap_prog, sizeof programmed);
        budget = (long)k;
        (void)boot(&lg);
        CHECK(dead);
        dead = false;
        budget = -1;
        cuts++;
        CHECK(boot(&lg));
        CHECK(lg.mount_status == NV_LOG_MIGRATED || lg.mount_status == NV_LOG_MOUNTED);
        check_model(&lg);
        CHECK(!old_layout_valid());
        CHECK(boot(&lg) && lg.mount_status == NV_LOG_MOUNTED);
        check_model(&lg);
    }
    CHECK(double_programs == 0);
    printf("store_migration: %u power cuts during the migration boot, 0 records lost\n", cuts);

    /* 4. Life goes on in the new layout: compactions use bank 1 (over the
     *    old area); the old geometry never becomes valid again. */
    memcpy(nor, snap_nor, sizeof nor);
    memcpy(programmed, snap_prog, sizeof programmed);
    CHECK(boot(&lg) && lg.mount_status == NV_LOG_MIGRATED);
    uint32_t c0 = lg.stats.compactions;
    for (unsigned r = 0; r < 4000; r++) write_rec(&lg, rnd() % 150u);
    CHECK(lg.stats.compactions >= c0 + 2);
    unsigned comps = (unsigned)(lg.stats.compactions - c0);
    check_model(&lg);
    CHECK(boot(&lg) && lg.mount_status == NV_LOG_MOUNTED);
    check_model(&lg);
    CHECK(!old_layout_valid());
    CHECK(double_programs == 0);
    printf("store_migration: %u compactions afterwards in 56 KiB banks, %u writes between compactions\n",
           comps, 4000u / comps);
    PASS("store_migration");
    return 0;
}
