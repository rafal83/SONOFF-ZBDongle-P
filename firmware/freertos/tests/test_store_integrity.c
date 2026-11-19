/* SPDX-License-Identifier: MIT
 * Persistence integrity of the coordinator (zb_coord + nv_log):
 *  A. restore refuses a network record whose counters are missing, malformed,
 *     bound to another key or incoherent (the defect: counters restarted at
 *     zero under the same key); legacy records still accepted and upgraded;
 *  B. a corrupted store is never formatted implicitly;
 *  C. a power cut at EVERY flash operation of a full scenario (formation, 40
 *     devices, several NWK and TC APS counter reservations, counter raises,
 *     compactions): after the reboot the store mounts, and the restored
 *     coordinator keeps the network key, TCLK seed and link keys and never
 *     reuses an outgoing NWK or APS frame counter.
 */
#include "frt_test.h"
#include "zb_coord.h"
#include "nv_log.h"

/* ---- simulated NOR with power cuts ----------------------------------- */
#define SECTOR 2048u
#define BANK (2u * SECTOR)
#define SIZE (2u * BANK)
static uint8_t nor[SIZE];
static long budget = -1;          /* flash operations before the cut; -1 = never */
static bool dead;
static unsigned flash_ops;
static bool op(void) {
    if (dead) return false;
    flash_ops++;
    if (budget < 0) return true;
    if (budget-- == 0) { dead = true; return false; }
    return true;
}
static bool f_read(void *ctx, uint32_t a, void *b, size_t n) { (void)ctx; if (dead) return false; memcpy(b, nor + a, n); return true; }
static bool f_prog(void *ctx, uint32_t a, const void *d, size_t n) {
    (void)ctx;
    bool ok = op();
    size_t limit = ok ? n : (dead ? frt_rand() % (n + 1) : 0);   /* torn write: a prefix */
    for (size_t i = 0; i < limit; i++) nor[a + i] &= ((const uint8_t *)d)[i];
    return ok;
}
static bool f_erase(void *ctx, uint32_t a) {
    (void)ctx;
    bool ok = op();
    size_t limit = ok ? SECTOR : (dead ? frt_rand() % SECTOR : 0);   /* interrupted erase */
    memset(nor + a, 0xFF, limit);
    return ok;
}
static const nv_flash_ops_t OPS = {f_read, f_prog, f_erase, 0};
static void power_restored(void) { dead = false; budget = -1; }

/* ---- radio: every frame "sent", MAC confirms immediately ------------- */
static zb_coord_t coord;
static unsigned on_air;
static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) { (void)ctx; (void)mpdu; (void)len; (void)ack; on_air++; return 0; }
static void sim_match(void *ctx, bool e, uint64_t a, bool p) { (void)ctx; (void)e; (void)a; (void)p; }
static const mac_radio_ops_t RADIO = {sim_tx, sim_match, 0};
static bool rnd(void *ctx, uint8_t k[16]) { (void)ctx; for (unsigned i = 0; i < 16; i++) k[i] = (uint8_t)(0x3C + 7 * i); return true; }
static const zb_coord_upper_t UPPER = {.random_key = rnd};
#define EXT 0x00124B0001020304ull
static const uint8_t KEY[16] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18, 0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90};
static const uint8_t EPID[8] = {8, 7, 6, 5, 4, 3, 2, 1};

static nv_log_t store;
static void boot(void) {
    memset(&coord, 0xA5, sizeof coord);   /* RAM lost */
    zb_coord_init(&coord, &RADIO, &UPPER, EXT, 0x1234u, 0);
    zb_coord_attach_store(&coord, &store);
}
static void drain_mac(void) { for (unsigned i = 0; i < 4; i++) zb_coord_tx_done(&coord, MAC_TX_OK, false); }

/* ---- A: restore validation (the defect) ----------------------------- */
static void part_a(void) {
    memset(nor, 0xFF, sizeof nor);
    power_restored();
    CHECK(nv_log_mount(&store, &OPS, 0, BANK, SECTOR) && store.mount_status == NV_LOG_FORMATTED);
    boot();
    zb_coord_form(&coord, 0x2B2B, 15, EPID, KEY, 0);
    /* Formation writes the counters BEFORE the network record. */
    uint8_t buf[NV_LOG_MAX_RECORD];
    uint16_t len = 0;
    CHECK(nv_log_read(&store, 2, buf, sizeof buf, &len) && len == 13 && buf[0] == 2);
    /* An authorized device and frames secured with the key. */
    CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, 0xA4C1380000000001ull, 0x1111, true, 0, 0xFF));
    const uint8_t zcl[3] = {1, 2, 3};
    for (unsigned i = 0; i < 5; i++) { CHECK(zb_coord_send_aps(&coord, 0x1111, 1, 1, 6, 0x104, zcl, 3, false) == ZB_SEND_OK); drain_mac(); }
    uint32_t used = coord.security.tx_next;
    CHECK(used > 0);

    /* 1. THE DEFECT: network record present, counters record lost. The old
     *    code restored with counters at 0 under the same key. */
    uint8_t counters[NV_LOG_MAX_RECORD];
    uint16_t counters_len = 0;
    CHECK(nv_log_read(&store, 2, counters, sizeof counters, &counters_len));
    CHECK(nv_log_delete(&store, 2));
    boot();
    CHECK(!zb_coord_restore(&coord) && coord.restore_status == ZB_RESTORE_COUNTERS_MISSING);
    CHECK(coord.security.tx_next == 0 && !coord.mac.started);            /* nothing applied */
    for (unsigned i = 0; i < 16; i++) CHECK(coord.network_key[i] == 0);
    CHECK(zb_coord_send_aps(&coord, 0x1111, 1, 1, 6, 0x104, zcl, 3, false) != ZB_SEND_OK);   /* cannot emit */
    uint8_t net[NV_LOG_MAX_RECORD];
    uint16_t net_len = 0;
    CHECK(nv_log_read(&store, 1, net, sizeof net, &net_len));          /* nothing erased */

    /* 2. Malformed counters. */
    CHECK(nv_log_write(&store, 2, counters, 5));
    boot();
    CHECK(!zb_coord_restore(&coord) && coord.restore_status == ZB_RESTORE_COUNTERS_INVALID);
    uint8_t bad[13];
    memcpy(bad, counters, 13);
    bad[0] = 7;                                                         /* unknown version */
    CHECK(nv_log_write(&store, 2, bad, 13));
    boot();
    CHECK(!zb_coord_restore(&coord) && coord.restore_status == ZB_RESTORE_COUNTERS_INVALID);

    /* 3. Counters bound to another key (network record from another key). */
    CHECK(nv_log_write(&store, 2, counters, counters_len));
    uint8_t other[NV_LOG_MAX_RECORD];
    memcpy(other, net, net_len);
    other[12] ^= 0x5A;                                                  /* key starts at offset 12 */
    CHECK(nv_log_write(&store, 1, other, net_len));
    boot();
    CHECK(!zb_coord_restore(&coord) && coord.restore_status == ZB_RESTORE_COUNTERS_OTHER_KEY);
    CHECK(nv_log_write(&store, 1, net, net_len));

    /* 4. Legacy 8-byte record at zero: legacy firmware wrote it only at the
     *    first durable reservation (>= 1001), so zero is not genuine. */
    uint8_t legacy[8] = {0};
    CHECK(nv_log_write(&store, 2, legacy, 8));
    boot();
    CHECK(!zb_coord_restore(&coord) && coord.restore_status == ZB_RESTORE_COUNTERS_INCOHERENT);
    /*    Legacy non-zero record: accepted, then rewritten as v2. */
    for (unsigned i = 0; i < 4; i++) { legacy[i] = counters[1 + i]; legacy[4 + i] = counters[5 + i]; }
    CHECK(nv_log_write(&store, 2, legacy, 8));
    boot();
    CHECK(zb_coord_restore(&coord) && coord.restore_status == ZB_RESTORE_OK);
    CHECK(nv_log_read(&store, 2, buf, sizeof buf, &len) && len == 13);

    /* 5. Valid record: resumes strictly above every counter used. */
    boot();
    CHECK(zb_coord_restore(&coord) && coord.security.tx_next >= used && !memcmp(coord.network_key, KEY, 16));
    CHECK(coord.stats.restored_devices == 1);
}

/* ---- B: corrupted store at boot -------------------------------------- */
static void part_b(void) {
    memset(nor, 0xFF, sizeof nor);
    power_restored();
    CHECK(nv_log_mount(&store, &OPS, 0, BANK, SECTOR));
    boot();
    zb_coord_form(&coord, 0x2B2B, 15, EPID, KEY, 0);
    nor[1] ^= 0x10;                         /* the only committed bank header is damaged */
    uint8_t copy[SIZE];
    memcpy(copy, nor, sizeof nor);
    CHECK(!nv_log_mount(&store, &OPS, 0, BANK, SECTOR) && store.mount_status == NV_LOG_ERR_CORRUPT);
    CHECK(!memcmp(copy, nor, sizeof nor));  /* untouched: the records remain recoverable */
    boot();
    CHECK(!zb_coord_restore(&coord) && coord.restore_status == ZB_RESTORE_NONE);
    nor[1] ^= 0x10;                         /* e.g. repaired from a dump: everything is back */
    CHECK(nv_log_mount(&store, &OPS, 0, BANK, SECTOR) && store.mount_status == NV_LOG_MOUNTED);
    boot();
    CHECK(zb_coord_restore(&coord) && !memcmp(coord.network_key, KEY, 16));
}

/* ---- C: power cut at every flash operation --------------------------- */
#define DEVICES 40u
static uint8_t seed_at_form[16];
static bool cut_seen(void) { return dead; }

/* Runs the scenario until the cut (returns false if it was cut). */
static bool scenario(uint32_t *nwk_used, uint32_t *aps_used, bool *formed) {
    *formed = false;
    zb_coord_form(&coord, 0x3C3C, 20, EPID, KEY, 0);
    if (cut_seen()) return false;
    *formed = true;
    memcpy(seed_at_form, coord.tclk_seed, 16);
    for (unsigned i = 0; i < DEVICES; i++) {
        uint8_t key[16];
        zb_tclk_derive(coord.tclk_seed, (uint8_t)(i % 16), 0xA4C1380000001000ull + i, key);
        (void)zb_coord_import_device(&coord, NWK_DEVICE_NONE, 0xA4C1380000001000ull + i, (uint16_t)(0x2000 + i), true,
                                     key, (uint8_t)(i % 16));
        if (cut_seen()) return false;
    }
    const uint8_t zcl[3] = {1, 2, 3};
    /* > 2 reservation windows of NWK counters. */
    for (unsigned i = 0; i < 2 * CORE_SECURITY_RESERVATION + 20; i++) {
        (void)zb_coord_send_aps(&coord, 0x2000, 1, 1, 6, 0x104, zcl, 3, false);
        drain_mac();
        *nwk_used = coord.security.tx_next;
        if (cut_seen()) return false;
    }
    /* > 1 reservation window of TC APS counters (APS-secured data). */
    for (unsigned i = 0; i < CORE_SECURITY_RESERVATION + 20; i++) {
        (void)zb_coord_send_aps_secured(&coord, 0x2001, 1, 1, 6, 0x104, zcl, 3, false);
        drain_mac();
        *nwk_used = coord.security.tx_next;
        *aps_used = coord.tc_aps_frame_counter;
        if (cut_seen()) return false;
    }
    /* Counter raises (as a backup restore does). */
    (void)zb_coord_raise_nwk_frame_counter(&coord, coord.security.tx_next + 5000u);
    if (cut_seen()) return false;
    *nwk_used = coord.security.tx_next;
    zb_coord_raise_aps_frame_counter(&coord, coord.tc_aps_frame_counter + 3000u);
    if (cut_seen()) return false;
    *aps_used = coord.tc_aps_frame_counter;
    /* Device rewrites: forces compactions of the small store. */
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned i = 0; i < DEVICES; i++) {
            uint16_t slot = nwk_devices_find_ext(&coord.devices, 0xA4C1380000001000ull + i);
            if (slot == NWK_DEVICE_NONE) continue;
            coord.devices.dev[slot].aps_in_counter += 1024u;   /* persisted counter window */
            coord.devices.dev[slot].aps_in_seen = true;
            zb_coord_device_changed(&coord, slot);
            if (cut_seen()) return false;
        }
    }
    return true;
}

static void part_c(void) {
    /* Count the flash operations of an uninterrupted run. */
    memset(nor, 0xFF, sizeof nor);
    power_restored();
    CHECK(nv_log_mount(&store, &OPS, 0, BANK, SECTOR));
    flash_ops = 0;                  /* scenario operations only */
    boot();
    uint32_t nwk_used = 0, aps_used = 0;
    bool formed;
    CHECK(scenario(&nwk_used, &aps_used, &formed));
    unsigned total = flash_ops;
    CHECK(store.stats.compactions >= 2);
    unsigned cuts = 0, restored = 0, not_formed = 0, compaction_cuts = 0;
    for (unsigned at = 0; at < total; at++) {
        memset(nor, 0xFF, sizeof nor);
        power_restored();
        CHECK(nv_log_mount(&store, &OPS, 0, BANK, SECTOR));
        boot();
        unsigned compactions_before = store.stats.compactions;
        budget = (long)at;              /* cut at the at-th flash operation of the scenario */
        flash_ops = 0;
        nwk_used = aps_used = 0;
        bool finished = scenario(&nwk_used, &aps_used, &formed);
        if (finished) break;
        cuts++;
        if (store.stats.compactions != compactions_before || store.full) compaction_cuts++;
        /* Snapshot of what may have been emitted: RAM counters at the cut. */
        uint32_t nwk_ram = coord.security.tx_next, aps_ram = coord.tc_aps_frame_counter;
        bool had_seed = formed;
        uint8_t keys[DEVICES][16];
        bool had_key[DEVICES];
        for (unsigned i = 0; i < DEVICES; i++) {
            uint16_t s = nwk_devices_find_ext(&coord.devices, 0xA4C1380000001000ull + i);
            had_key[i] = s != NWK_DEVICE_NONE && coord.devices.dev[s].unique_key;
            if (had_key[i]) memcpy(keys[i], coord.devices.dev[s].link_key, 16);
        }
        /* Power back: the store must mount (no corruption from a cut). */
        power_restored();
        CHECK(nv_log_mount(&store, &OPS, 0, BANK, SECTOR));
        CHECK(store.mount_status == NV_LOG_MOUNTED || store.mount_status == NV_LOG_FORMATTED);
        boot();
        bool ok = zb_coord_restore(&coord);
        if (!ok) {
            /* Only "no network yet" is acceptable, never a refused restore. */
            CHECK(coord.restore_status == ZB_RESTORE_NONE);
            not_formed++;
            continue;
        }
        restored++;
        /* Cryptographic invariants. */
        CHECK(!memcmp(coord.network_key, KEY, 16));
        CHECK(coord.security.tx_next >= nwk_ram && coord.security.tx_next >= nwk_used);   /* no NWK nonce reuse */
        CHECK(coord.tc_aps_frame_counter >= aps_ram && coord.tc_aps_frame_counter >= aps_used);   /* no APS reuse */
        if (had_seed && coord.tclk_seed_valid) CHECK(!memcmp(coord.tclk_seed, seed_at_form, 16));
        for (unsigned i = 0; i < DEVICES; i++) {
            uint16_t s = nwk_devices_find_ext(&coord.devices, 0xA4C1380000001000ull + i);
            if (s == NWK_DEVICE_NONE || !coord.devices.dev[s].unique_key) continue;
            /* A restored unique key is always the key the device holds. */
            uint8_t expect[16];
            zb_tclk_derive(coord.tclk_seed, (uint8_t)(i % 16), 0xA4C1380000001000ull + i, expect);
            CHECK(!memcmp(coord.devices.dev[s].link_key, expect, 16));
            if (had_key[i]) CHECK(!memcmp(coord.devices.dev[s].link_key, keys[i], 16));
        }
        /* The very next frame uses a counter never used before the cut. */
        const uint8_t zcl[3] = {9, 9, 9};
        uint32_t before = coord.security.tx_next;
        if (zb_coord_send_aps(&coord, 0x2000, 1, 1, 6, 0x104, zcl, 3, false) == ZB_SEND_OK)
            CHECK(before >= nwk_ram);
        drain_mac();
    }
    CHECK(cuts == total);
    CHECK(restored > 0 && compaction_cuts > 0);
    printf("store_integrity: %u flash ops, %u cuts (%u restored, %u before the first network record, "
           "%u around compactions)\n", total, cuts, restored, not_formed, compaction_cuts);
}

/* ---- D: counter floors across re-formation and recovery format ------- */
static uint32_t record_nwk(void) {
    uint8_t b[16];
    uint16_t len = 0;
    CHECK(nv_log_read(&store, 2, b, sizeof b, &len) && (len == 13 || len == 8));
    const uint8_t *p = len == 13 ? b + 1 : b;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void part_d(void) {
    /* D1. Reset of the adapter state (ZNP CLEAR_STATE: forget + re-init)
     * then formation with the SAME key, as Zigbee2MQTT does with the
     * network_key of its configuration: counters must not restart. */
    memset(nor, 0xFF, sizeof nor);
    power_restored();
    CHECK(nv_log_mount(&store, &OPS, 0, BANK, SECTOR));
    boot();
    CHECK(!coord.counter_floor_from_store);
    zb_coord_form(&coord, 0x2B2B, 15, EPID, KEY, 0);
    CHECK(zb_coord_raise_nwk_frame_counter(&coord, 70000u));
    zb_coord_raise_aps_frame_counter(&coord, 9000u);
    uint32_t used_nwk = coord.security.tx_next, used_aps = coord.tc_aps_frame_counter;
    zb_coord_forget(&coord);
    boot();
    CHECK(coord.counter_floor_from_store);
    CHECK(coord.security.tx_next >= used_nwk && coord.tc_aps_frame_counter >= used_aps);
    zb_coord_form(&coord, 0x2B2B, 15, EPID, KEY, 0);
    CHECK(record_nwk() >= used_nwk);
    boot();
    CHECK(zb_coord_restore(&coord) && coord.security.tx_next >= used_nwk);

    /* D2. Many counters records spread over both banks (compactions), then
     * both bank headers damaged: the store is CORRUPT, the recovery format
     * salvages the highest committed counters and keeps them as a floor. A
     * record whose CRC fails is never used, even with a higher value. */
    uint32_t v = used_nwk;
    for (unsigned i = 0; i < 300; i++) { v += 1000u; CHECK(zb_coord_raise_nwk_frame_counter(&coord, v)); }
    CHECK(store.stats.compactions >= 1);
    uint32_t best = coord.security.tx_next;
    CHECK(zb_coord_raise_nwk_frame_counter(&coord, best + 0x100000u));   /* then made invalid */
    uint32_t rec = (store.active ? BANK : 0u) + store.write_offset - 28u;  /* last record: 8 + 16 + 4 */
    nor[rec + 8 + 2] ^= 0x01;                                             /* bit flip in its NWK value */
    nor[1] ^= 0x10;
    nor[BANK + 1] ^= 0x10;
    CHECK(!nv_log_mount(&store, &OPS, 0, BANK, SECTOR) && store.mount_status == NV_LOG_ERR_CORRUPT);
    uint32_t floor_nwk = 0, floor_aps = 0;
    CHECK(zb_coord_store_format_keep_counters(&store, &floor_nwk, &floor_aps));
    CHECK(floor_nwk == best && floor_aps >= used_aps);
    CHECK(store.mounted && record_nwk() == best);
    boot();
    CHECK(coord.counter_floor_from_store && coord.security.tx_next == best);
    CHECK(!zb_coord_restore(&coord) && coord.restore_status == ZB_RESTORE_NONE);   /* no network: host restores */
    zb_coord_form(&coord, 0x2B2B, 15, EPID, KEY, 0);                              /* herdsman's provisional one */
    CHECK(record_nwk() >= best);

    /* D3. A blank store keeps no floor: nothing to salvage, nothing written. */
    memset(nor, 0xFF, sizeof nor);
    store.ops = OPS; store.base = 0; store.bank_size = BANK; store.sector_size = SECTOR;
    CHECK(zb_coord_store_format_keep_counters(&store, &floor_nwk, &floor_aps) && floor_nwk == 0 && floor_aps == 0);
    uint16_t len;
    uint8_t b[16];
    CHECK(!nv_log_read(&store, 2, b, sizeof b, &len));
    printf("store_integrity: re-formation with the same key keeps counters >= %u; corrupted store: floor %u "
           "salvaged (a higher record with a bad CRC ignored)\n", (unsigned)used_nwk, (unsigned)best);
}

int main(void) {
    part_a();
    part_b();
    part_c();
    part_d();
    PASS("store_integrity");
    return 0;
}
