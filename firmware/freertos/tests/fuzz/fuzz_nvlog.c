/* SPDX-License-Identifier: MIT
 * libFuzzer: the nv_log store on arbitrary Flash contents (corruption,
 * hostile or torn data): mount, read every id, salvage the counters, write,
 * compact; and the coordinator restore on top of it.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nv_log.h"
#include "zb_coord.h"

#define SECTOR 1024u
#define BANK (2u * SECTOR)
static uint8_t nor[2u * BANK];

static bool f_read(void *ctx, uint32_t a, void *b, size_t n) {
    (void)ctx;
    if (a + n > sizeof nor) return false;
    memcpy(b, nor + a, n);
    return true;
}
static bool f_prog(void *ctx, uint32_t a, const void *d, size_t n) {
    (void)ctx;
    if (a + n > sizeof nor) return false;
    for (size_t i = 0; i < n; i++) nor[a + i] &= ((const uint8_t *)d)[i];
    return true;
}
static bool f_erase(void *ctx, uint32_t a) {
    (void)ctx;
    if (a % SECTOR || a + SECTOR > sizeof nor) return false;
    memset(nor + a, 0xFF, SECTOR);
    return true;
}
static void salvage(void *ctx, const uint8_t *d, uint16_t len) { (void)ctx; (void)d; (void)len; }

static int tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) { (void)ctx; (void)mpdu; (void)len; (void)ack; return 0; }
static void match(void *ctx, bool e, uint64_t a, bool p) { (void)ctx; (void)e; (void)a; (void)p; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static const nv_flash_ops_t ops = {f_read, f_prog, f_erase, 0};
    static const mac_radio_ops_t radio = {tx, match, 0};
    static nv_log_t log;
    static zb_coord_t coord;
    memset(nor, 0xFF, sizeof nor);
    memcpy(nor, data, size < sizeof nor ? size : sizeof nor);
    bool mounted = nv_log_mount(&log, &ops, 0, BANK, SECTOR);
    log.ops = ops; log.base = 0; log.bank_size = BANK; log.sector_size = SECTOR;
    (void)nv_log_salvage(&log, 2, salvage, 0);
    if (mounted) {
        uint8_t buf[NV_LOG_MAX_RECORD];
        uint16_t len;
        for (uint16_t id = 0; id < NV_LOG_MAX_IDS; id++) (void)nv_log_read(&log, id, buf, sizeof buf, &len);
        zb_coord_init(&coord, &radio, 0, 0x00124B0001020304ull, 1, 0);
        zb_coord_attach_store(&coord, &log);
        (void)zb_coord_restore(&coord);
        /* Writes until a compaction happens. */
        for (unsigned i = 0; i < 40; i++) {
            uint8_t rec[40];
            memset(rec, (int)i, sizeof rec);
            if (!nv_log_write(&log, (uint16_t)(300 + (i % 7)), rec, sizeof rec)) break;
        }
    } else {
        uint32_t nwk, aps;
        (void)zb_coord_store_format_keep_counters(&log, &nwk, &aps);
    }
    return 0;
}
