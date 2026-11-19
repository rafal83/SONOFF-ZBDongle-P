/* SPDX-License-Identifier: MIT
 * Original bounded NV read cache. The NV driver owns synchronization.
 */
#ifndef PORT_NV_CACHE_H
#define PORT_NV_CACHE_H
#include <stdbool.h>
#include <stdint.h>
#define PORT_NV_CACHE_SLOTS 4u
#define PORT_NV_CACHE_BYTES 64u
typedef struct {
    uint32_t hits, misses, bypasses, invalidations, identical_writes, content_hits;
} port_nv_stats_t;
extern port_nv_stats_t port_nv_stats;
bool port_nv_cache_get(uint8_t system, uint16_t item, uint16_t sub,
                       uint16_t offset, uint16_t length, void *buffer);
void port_nv_cache_put(uint8_t system, uint16_t item, uint16_t sub,
                       uint16_t offset, uint16_t length, const void *buffer);
/* Call before EVERY attempted physical program/erase, including compaction,
 * delete, erase-all and failed writes. Never caches negative lookup results.
 */
void port_nv_cache_invalidate(void);
typedef struct {
    uint32_t invalidations;
    uint16_t epoch;
} port_nv_cache_checkpoint;
port_nv_cache_checkpoint port_nv_cache_checkpoint_get(void);
/* NV mutex held. Only after a verified successful single-record transaction,
 * with no erase, compaction, nested mutation or changed content-index shape.
 * Restore unrelated entries; never restore the written ID or content family.
 * Invalidation before each physical operation remains mandatory. */
void port_nv_cache_resume(const port_nv_cache_checkpoint *checkpoint,
                          uint8_t system, uint16_t item, uint16_t sub);
/* Content-addressed hot lookups (e.g. EUI64/TCLK) share the SAME fixed pool.
 * Only SUCCESS and NOTFOUND may be stored; all tags are verified on a hit.
 */
bool port_nv_content_get(uint8_t system, uint16_t item, uint16_t sub, uint16_t offset,
    uint16_t length, uint16_t coff, uint16_t clen, const void *query, void *buffer,
    uint16_t *found_sub, uint8_t *status);
void port_nv_content_put(uint8_t system, uint16_t item, uint16_t sub, uint16_t offset,
    uint16_t length, uint16_t coff, uint16_t clen, const void *query, const void *buffer,
    uint16_t found_sub, uint8_t status);
#endif
