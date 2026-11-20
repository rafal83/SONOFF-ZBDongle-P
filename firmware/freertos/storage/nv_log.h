/* SPDX-License-Identifier: MIT
 * Power-fail-safe key/value log for NOR flash (two banks, append-only).
 *
 * Layout: two banks of `bank_size` bytes (multiple of `sector_size`). Each
 * bank starts with a 16-byte header {magic, version, generation, complete}.
 * Records are appended at 4-byte aligned offsets:
 *   id u16 | len u16 | hcrc u16 | 0xFFFF | data (padded to 4) | crc32 u32
 * hcrc protects id/len so a torn header never yields a bogus length; the
 * trailing crc32 (over id, len, data) commits the record: it is written
 * last, so a record interrupted by a power loss is ignored at mount.
 * Compaction copies the latest live records into the other bank, then
 * programs `complete`; until then the old bank remains authoritative.
 *
 * Every write returns only after the record is durable. NOR rules are
 * respected: each byte is programmed at most once between erases.
 * Bounded work: mount scans the active bank once; reads are O(1) through a
 * RAM index of NV_LOG_MAX_IDS offsets; compaction is O(live data).
 */
#ifndef NV_LOG_H
#define NV_LOG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NV_LOG_MAX_IDS 512u
#define NV_LOG_MAX_RECORD 256u
#define NV_LOG_HEADER 16u

typedef struct {
    bool (*read)(void *ctx, uint32_t address, void *buffer, size_t length);
    bool (*program)(void *ctx, uint32_t address, const void *data, size_t length);
    bool (*erase_sector)(void *ctx, uint32_t address);
    void *ctx;
} nv_flash_ops_t;

typedef struct {
    uint32_t mounts, writes, compactions, torn_records, torn_headers, erase_failures, program_failures;
    uint32_t old_layout_retired;
} nv_log_stats_t;

/* Outcome of nv_log_mount(). Only a store holding no committed data
 * (both banks erased, or carrying nothing but an uncommitted, empty header
 * left by an interrupted format) is formatted automatically. Anything else
 * that is not a valid log is left untouched and reported: recovery is an
 * explicit nv_log_format() decided by the operator. */
typedef enum {
    NV_LOG_MOUNTED = 0,        /* existing log mounted */
    NV_LOG_FORMATTED = 1,      /* blank store, formatted */
    NV_LOG_ERR_CORRUPT = 2,    /* data present but no valid bank: nothing erased */
    NV_LOG_ERR_IO = 3,         /* flash read/erase/program failure */
    NV_LOG_ERR_PARAM = 4,
    NV_LOG_UNMOUNTED = 5,
    NV_LOG_MIGRATED = 6,       /* old storage layout copied into this one at this boot */
} nv_log_mount_status_t;

typedef struct {
    nv_flash_ops_t ops;
    uint32_t base, bank_size, sector_size;
    uint8_t active;                 /* 0 or 1 */
    uint32_t generation;
    uint32_t write_offset;          /* next free offset inside the active bank */
    bool full;                      /* torn header seen: compact before writing */
    uint16_t index[NV_LOG_MAX_IDS]; /* offset/4 of the latest record, 0 = absent */
    /* Scratch owned by the log (single owner task): keeps the work off the
     * caller's stack (compaction would otherwise need ~2 KiB of stack). */
    uint16_t scratch_index[NV_LOG_MAX_IDS];
    uint8_t tmp[NV_LOG_MAX_RECORD], tmp2[NV_LOG_MAX_RECORD];
    nv_log_stats_t stats;
    nv_log_mount_status_t mount_status;
    bool mounted;                   /* writes/reads refused while false */
    uint16_t migrated_records;      /* records copied by nv_log_migrate() */
} nv_log_t;

/* Mount. Formats ONLY a store without committed data (see
 * nv_log_mount_status_t); returns false otherwise, nothing erased. */
bool nv_log_mount(nv_log_t *log, const nv_flash_ops_t *ops, uint32_t base, uint32_t bank_size,
                  uint32_t sector_size);
/* Durable write; len <= NV_LOG_MAX_RECORD. Identical content is not rewritten. */
bool nv_log_write(nv_log_t *log, uint16_t id, const void *data, uint16_t len);
bool nv_log_delete(nv_log_t *log, uint16_t id);
/* Returns false if absent; *len receives the stored length (copied up to cap). */
bool nv_log_read(nv_log_t *log, uint16_t id, void *buffer, uint16_t cap, uint16_t *len);
/* Every committed (CRC-valid) version of record `id` found in EITHER bank,
 * whatever the state of the bank headers, mounted or not (ops/base/sizes
 * must be set): recovery of monotonic values (frame counters) before an
 * explicit format of a corrupted store. Read-only. Returns the count. */
typedef void (*nv_log_salvage_fn)(void *ctx, const uint8_t *data, uint16_t len);
unsigned nv_log_salvage(nv_log_t *log, uint16_t id, nv_log_salvage_fn fn, void *ctx);
/* Erase both banks and start an empty log (factory reset of the store). */
bool nv_log_format(nv_log_t *log);

/* Storage layout change (bigger banks elsewhere). When the new geometry holds
 * no valid store but the OLD geometry does: every live record of the old
 * store is copied into bank 0 of the new geometry (which must not overlap
 * the old store), committed last, then the old store's headers are erased so
 * that an older firmware finds no valid store instead of stale data. A power
 * cut before the commit leaves the old store intact: the migration is simply
 * redone at the next boot. On NV_LOG_MIGRATION_DONE `log` is mounted (status
 * NV_LOG_MIGRATED). NONE: no old store either. */
typedef enum { NV_LOG_MIGRATION_NONE, NV_LOG_MIGRATION_DONE, NV_LOG_MIGRATION_ERROR } nv_log_migration_t;
nv_log_migration_t nv_log_migrate(nv_log_t *log, const nv_flash_ops_t *ops, uint32_t old_base, uint32_t old_bank_size,
                                  uint32_t base, uint32_t bank_size, uint32_t sector_size);
/* On a mounted new-layout store: erase what is left of the old layout's
 * headers, only while the new bank covering it has never been used (true
 * if something was erased). Finishes a migration interrupted after commit. */
bool nv_log_retire_old(nv_log_t *log, uint32_t old_base, uint32_t old_bank_size);
uint32_t nv_log_crc32(uint32_t crc, const void *data, size_t length);
#endif
