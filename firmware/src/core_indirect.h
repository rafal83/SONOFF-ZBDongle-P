/* SPDX-License-Identifier: MIT
 * Independent indirect-buffer accounting and per-destination FIFO.
 * One owner task. Payload memory remains owned by the caller.
 */
#ifndef CORE_INDIRECT_H
#define CORE_INDIRECT_H
#include "core_index.h"
#include <stddef.h>
#define CORE_INDIRECT_CAPACITY 24
#define CORE_INDIRECT_PER_CHILD 3
#define CORE_INDIRECT_BROADCASTS 3
#define CORE_INDIRECT_CHILD_WORDS 4
typedef uint64_t core_indirect_token_t;
#define CORE_INDIRECT_NONE UINT64_MAX
typedef struct { uint8_t child, prev, next, state, free_next; uint32_t generation; void *payload; } core_indirect_record_t;
typedef struct { uint16_t address; uint8_t count, head, tail, free_next, in_flight; } core_indirect_child_t;
typedef struct {
    uint64_t order;
    uint32_t pending[CORE_INDIRECT_CHILD_WORDS];
    uint16_t in_flight_address;
    uint8_t slot, in_flight_peer;
    bool used;
} core_indirect_broadcast_t;
typedef struct {
    core_index_t addresses;
    core_index_branch_t branches[CORE_INDIRECT_CAPACITY-1];
    uint64_t keys[CORE_INDIRECT_CAPACITY];
    uint8_t occupied[(CORE_INDIRECT_CAPACITY+7)/8];
    core_indirect_record_t records[CORE_INDIRECT_CAPACITY];
    core_indirect_child_t children[CORE_INDIRECT_CAPACITY];
    core_indirect_broadcast_t broadcasts[CORE_INDIRECT_BROADCASTS];
    uint64_t broadcast_order;
    uint8_t free_record, free_child, count;
} core_indirect_t;
void core_indirect_init(core_indirect_t *);
core_indirect_token_t core_indirect_enqueue(core_indirect_t *, uint16_t address, void *payload);
core_indirect_token_t core_indirect_poll(core_indirect_t *, uint16_t address, void **payload);
bool core_indirect_retry(core_indirect_t *, core_indirect_token_t token);
bool core_indirect_release(core_indirect_t *, core_indirect_token_t token, void **payload);
uint8_t core_indirect_count(core_indirect_t *, uint16_t address);
core_indirect_token_t core_indirect_broadcast_enqueue(core_indirect_t *, void *payload,
    const uint32_t recipients[CORE_INDIRECT_CHILD_WORDS]);
core_indirect_token_t core_indirect_broadcast_poll(core_indirect_t *, uint16_t address, uint8_t peer, void **payload);
bool core_indirect_broadcast_confirm(core_indirect_t *, core_indirect_token_t, bool delivered);
uint8_t core_indirect_broadcast_pending(core_indirect_t *, core_indirect_token_t);
void core_indirect_broadcast_forget_peer(core_indirect_t *, uint8_t peer);
#endif
