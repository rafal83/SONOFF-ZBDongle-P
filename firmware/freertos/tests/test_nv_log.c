/* SPDX-License-Identifier: MIT
 * nv_log on a simulated NOR flash with power cuts at every possible step.
 */
#include "frt_test.h"
#include "nv_log.h"

#define SECTOR 512u
#define BANK (2u * SECTOR)
#define BASE 0x1000u
#define SIZE (2u * BANK)

static uint8_t nor[SIZE];
static uint8_t programmed[SIZE];     /* bytes programmed since their last erase */
static long budget = -1;             /* operations left before the power cut; -1 = none */
static bool dead;
static unsigned double_programs, ops;

static bool tick(void) {
    if (dead) return false;
    ops++;
    if (budget < 0) return true;
    if (budget-- == 0) { dead = true; return false; }
    return true;
}
static bool f_read(void *ctx, uint32_t a, void *buf, size_t n) {
    (void)ctx;
    if (dead) return false;
    CHECK(a >= BASE && a + n <= BASE + SIZE);
    memcpy(buf, nor + (a - BASE), n);
    return true;
}
static bool f_program(void *ctx, uint32_t a, const void *data, size_t n) {
    (void)ctx;
    CHECK(a >= BASE && a + n <= BASE + SIZE);
    const uint8_t *d = data;
    size_t limit = n;
    bool ok = tick();
    if (!ok && !dead) return false;
    if (!ok) limit = frt_rand() % (n + 1);             /* torn: only a prefix */
    for (size_t i = 0; i < limit; i++) {
        size_t o = a - BASE + i;
        if (programmed[o]) double_programs++;
        uint8_t v = d[i];
        if (!ok && i + 1 == limit) v |= (uint8_t)frt_rand();   /* last byte partly programmed */
        nor[o] &= v;
        programmed[o] = 1;
    }
    return ok;
}
static bool f_erase(void *ctx, uint32_t a) {
    (void)ctx;
    CHECK(a >= BASE && (a - BASE) % SECTOR == 0);
    bool ok = tick();
    size_t o = a - BASE;
    size_t limit = ok ? SECTOR : frt_rand() % SECTOR;   /* interrupted erase */
    for (size_t i = 0; i < limit; i++) { nor[o + i] = 0xFF; programmed[o + i] = 0; }
    return ok;
}
static const nv_flash_ops_t OPS = {f_read, f_program, f_erase, 0};

#define IDS 10u
static uint8_t model[IDS][64];
static uint16_t model_len[IDS];
static bool model_present[IDS];
static nv_log_t lg;

static void verify_all(void) {
    for (unsigned id = 0; id < IDS; id++) {
        uint8_t buf[NV_LOG_MAX_RECORD];
        uint16_t len = 0;
        bool present = nv_log_read(&lg, (uint16_t)id, buf, sizeof buf, &len);
        CHECK(present == model_present[id]);
        if (present) CHECK(len == model_len[id] && !memcmp(buf, model[id], len));
    }
}

int main(void) {
    /* Non-blank content without a valid bank: refused, NOTHING erased. */
    memset(nor, 0x55, sizeof nor);                     /* garbage before first use */
    unsigned before_ops = ops;
    CHECK(!nv_log_mount(&lg, &OPS, BASE, BANK, SECTOR) && lg.mount_status == NV_LOG_ERR_CORRUPT);
    CHECK(ops == before_ops);                          /* no program, no erase */
    for (unsigned i = 0; i < SIZE; i++) if (nor[i] != 0x55) { CHECK(0); break; }
    CHECK(!nv_log_write(&lg, 1, "x", 1));              /* unmounted: refused */
    /* Explicit recovery decided by the operator. */
    CHECK(nv_log_format(&lg) && lg.mount_status == NV_LOG_FORMATTED);
    /* A blank store is formatted automatically. */
    memset(nor, 0xFF, sizeof nor);
    memset(programmed, 0, sizeof programmed);
    CHECK(nv_log_mount(&lg, &OPS, BASE, BANK, SECTOR) && lg.mount_status == NV_LOG_FORMATTED);
    CHECK(lg.stats.mounts == 1 && lg.write_offset == NV_LOG_HEADER);
    /* A single flipped bit in the only committed header: corruption, kept. */
    {
        CHECK(nv_log_write(&lg, 9, "keep", 4));
        nor[0] ^= 0x01;
        nv_log_t probe;
        CHECK(!nv_log_mount(&probe, &OPS, BASE, BANK, SECTOR) && probe.mount_status == NV_LOG_ERR_CORRUPT);
        nor[0] ^= 0x01;                                  /* repaired: data intact */
        CHECK(nv_log_mount(&probe, &OPS, BASE, BANK, SECTOR) && probe.mount_status == NV_LOG_MOUNTED);
        uint8_t b[8]; uint16_t n = 0;
        CHECK(nv_log_read(&probe, 9, b, sizeof b, &n) && n == 4 && !memcmp(b, "keep", 4));
        CHECK(nv_log_delete(&probe, 9));
        CHECK(nv_log_mount(&lg, &OPS, BASE, BANK, SECTOR));
    }
    /* Basic operations. */
    const uint8_t a[5] = {1, 2, 3, 4, 5};
    CHECK(nv_log_write(&lg, 3, a, 5));
    uint8_t buf[NV_LOG_MAX_RECORD];
    uint16_t len;
    CHECK(nv_log_read(&lg, 3, buf, sizeof buf, &len) && len == 5 && !memcmp(buf, a, 5));
    unsigned writes = lg.stats.writes;
    CHECK(nv_log_write(&lg, 3, a, 5) && lg.stats.writes == writes);      /* identical: no wear */
    CHECK(nv_log_delete(&lg, 3) && !nv_log_read(&lg, 3, buf, sizeof buf, &len));
    CHECK(!nv_log_write(&lg, NV_LOG_MAX_IDS, a, 1) && !nv_log_write(&lg, 1, a, NV_LOG_MAX_RECORD + 1));
    CHECK(nv_log_write(&lg, 7, 0, 0) && nv_log_read(&lg, 7, buf, sizeof buf, &len) && len == 0);
    CHECK(nv_log_delete(&lg, 7));
    /* Remount keeps everything; compaction preserves the latest values. */
    for (unsigned round = 0; round < 300; round++) {
        unsigned id = round % IDS;
        model_len[id] = (uint16_t)(1 + frt_rand() % 48);
        for (unsigned k = 0; k < model_len[id]; k++) model[id][k] = (uint8_t)frt_rand();
        model_present[id] = true;
        CHECK(nv_log_write(&lg, (uint16_t)id, model[id], model_len[id]));
    }
    CHECK(lg.stats.compactions > 5);
    verify_all();
    CHECK(nv_log_mount(&lg, &OPS, BASE, BANK, SECTOR));
    verify_all();
    CHECK(double_programs == 0);

    /* Power cuts at random operations: after each "reboot" every record is
     * either its previous value or the value being written; others intact. */
    unsigned cuts = 0, rolled_forward = 0, rolled_back = 0;
    for (unsigned round = 0; round < 20000; round++) {
        unsigned id = frt_rand() % IDS;
        bool del = frt_rand() % 10 == 0;
        uint8_t value[64];
        uint16_t vlen = (uint16_t)(frt_rand() % 49);
        for (unsigned k = 0; k < vlen; k++) value[k] = (uint8_t)frt_rand();
        budget = (frt_rand() % 3 == 0) ? (long)(frt_rand() % 40) : -1;
        bool ok = del ? nv_log_delete(&lg, (uint16_t)id) : nv_log_write(&lg, (uint16_t)id, value, vlen);
        if (!dead) {
            budget = -1;
            CHECK(ok);
            if (del) model_present[id] = false;
            else { model_present[id] = true; model_len[id] = vlen; memcpy(model[id], value, vlen); }
            if (round % 97 == 0) verify_all();
            continue;
        }
        /* Power loss, then reboot. */
        cuts++;
        dead = false;
        budget = -1;
        CHECK(nv_log_mount(&lg, &OPS, BASE, BANK, SECTOR));
        uint16_t got_len = 0;
        bool present = nv_log_read(&lg, (uint16_t)id, buf, sizeof buf, &got_len);
        bool is_new = del ? !present : (present && got_len == vlen && !memcmp(buf, value, vlen));
        bool is_old = present == model_present[id] &&
                      (!present || (got_len == model_len[id] && !memcmp(buf, model[id], got_len)));
        CHECK(is_new || is_old);
        if (is_new && !is_old) {
            rolled_forward++;
            if (del) model_present[id] = false;
            else { model_present[id] = true; model_len[id] = vlen; memcpy(model[id], value, vlen); }
        } else {
            rolled_back++;
        }
        verify_all();
    }
    printf("nv_log: cuts=%u forward=%u back=%u compactions=%u\n", cuts, rolled_forward, rolled_back,
           lg.stats.compactions);
    CHECK(cuts > 500 && rolled_forward > 0 && rolled_back > 0);
    CHECK(double_programs == 0);
    /* A torn write is never followed by data in the same place: further
     * writes after a cut still succeed and survive a remount. */
    CHECK(nv_log_write(&lg, 1, a, 5) && nv_log_mount(&lg, &OPS, BASE, BANK, SECTOR));
    CHECK(nv_log_read(&lg, 1, buf, sizeof buf, &len) && len == 5 && !memcmp(buf, a, 5));
    printf("nv_log: %u cuts (%u forward, %u back), %u compactions, %u ops\n", cuts, rolled_forward,
           rolled_back, lg.stats.compactions, ops);
    PASS("nv_log");
    return 0;
}
