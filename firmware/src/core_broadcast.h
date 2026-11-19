/* SPDX-License-Identifier: MIT
 * BTT lifecycle, duplicate/echo index, unique passive ACKs and retry deadlines.
 * Input must have passed NWK validation/authentication. Single owner task.
 */
#ifndef CORE_BROADCAST_H
#define CORE_BROADCAST_H
#include "core_index.h"
#define CORE_BROADCAST_CAPACITY 40
#define CORE_BROADCAST_PEERS 128
#define CORE_BROADCAST_WORDS (CORE_BROADCAST_PEERS/32)
typedef struct {
    uint32_t pending[CORE_BROADCAST_WORDS];
    uint8_t expiry_prev, expiry_next, retry_prev, retry_next;
    uint8_t expiry_bucket, retry_bucket, retries, ack_ticks;
    uint8_t ack_goal;
    uint8_t used:1, waiting_confirmation:1, retry_scheduled:1, completed:1, expiry_round:1, retry_round:1;
} core_broadcast_record_t;
typedef struct {
    core_index_t transactions;
    core_index_branch_t branches[CORE_BROADCAST_CAPACITY-1];
    uint64_t keys[CORE_BROADCAST_CAPACITY];
    uint8_t occupied[(CORE_BROADCAST_CAPACITY+7)/8];
    core_broadcast_record_t records[CORE_BROADCAST_CAPACITY];
    uint8_t expiry_heads[128], retry_heads[128], free_next[CORE_BROADCAST_CAPACITY], free_head;
    uint32_t now;
} core_broadcast_t;
typedef struct { uint64_t expired, retry, complete; } core_broadcast_events_t;
typedef enum { CORE_BCAST_NEW, CORE_BCAST_DUPLICATE, CORE_BCAST_FULL, CORE_BCAST_INVALID } core_broadcast_result_t;
void core_broadcast_init(core_broadcast_t *, uint32_t now);
core_broadcast_result_t core_broadcast_admit(core_broadcast_t *, uint16_t source,
    uint8_t sequence, uint8_t lifetime_ticks, uint8_t ack_ticks, uint8_t retries,
    const uint32_t peers[CORE_BROADCAST_WORDS], uint16_t *slot);
bool core_broadcast_ack(core_broadcast_t *, uint16_t source, uint8_t sequence, uint8_t peer);
bool core_broadcast_confirm(core_broadcast_t *, uint16_t source, uint8_t sequence);
void core_broadcast_tick(core_broadcast_t *, core_broadcast_events_t *);
bool core_broadcast_cancel(core_broadcast_t *, uint16_t source, uint8_t sequence);
void core_broadcast_limit_acks(core_broadcast_t *, uint16_t source, uint8_t sequence, uint8_t limit);
void core_broadcast_forget_peer(core_broadcast_t *, uint8_t peer);
#endif
