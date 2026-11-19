/* SPDX-License-Identifier: MIT
 * Fixed-capacity Patricia index. At most 64 branches, regardless of occupancy.
 * Keys are identifiers, never cryptographic secrets. No cycle-constant claim.
 */
#ifndef CORE_INDEX_H
#define CORE_INDEX_H
#include <stdbool.h>
#include <stdint.h>
#define CORE_INDEX_NONE UINT16_MAX
#define CORE_INDEX_LEAF UINT16_C(0x8000)
typedef struct {
    uint16_t child[2];
    uint8_t bit;
} core_index_branch_t;
typedef struct {
    core_index_branch_t *branches;
    uint64_t *keys;
    uint8_t *occupied;
    uint16_t root, free_branch, capacity, count;
    uint8_t last_steps;
} core_index_t;
bool core_index_init(core_index_t *, core_index_branch_t *, uint64_t *, uint8_t *, uint16_t);
uint16_t core_index_find(core_index_t *, uint64_t);
bool core_index_insert(core_index_t *, uint64_t, uint16_t);
uint16_t core_index_remove(core_index_t *, uint64_t);
#endif
