/* SPDX-License-Identifier: MIT */
#ifndef PORT_NV_INDEX_H
#define PORT_NV_INDEX_H
#include <stdbool.h>
#include <stdint.h>
#define PORT_NV_INDEX_SLOTS 1024u
#define PORT_NV_INDEX_STASH 8u
#define PORT_NV_INDEX_CANDIDATES (4u + PORT_NV_INDEX_STASH)
typedef struct { uint32_t builds, lookups, overflows, generation, barriers; } port_nv_index_stats_t;
extern port_nv_index_stats_t port_nv_index_stats;
void port_nv_index_invalidate(void);
void port_nv_index_barrier(void);
bool port_nv_index_ready(void);
bool port_nv_index_blocked(void);
void port_nv_index_begin(void);
void port_nv_index_finish(bool complete);
/* All calls are serialized by NVOCMP's existing mutex. Location is pg<<13|ofs.
 * Equal content fingerprints can coexist; caller verifies full query bytes.
 */
unsigned port_nv_index_get(uint32_t key, uint16_t locations[PORT_NV_INDEX_CANDIDATES]);
bool port_nv_index_put(uint32_t key, uint16_t location, bool replace);
void port_nv_index_remove(uint32_t key, uint16_t location);
#endif
