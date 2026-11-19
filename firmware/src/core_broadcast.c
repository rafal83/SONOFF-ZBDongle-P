/* SPDX-License-Identifier: MIT */
#include "core_broadcast.h"
#include <string.h>
#define NONE UINT8_MAX
static uint64_t key(uint16_t source, uint8_t sequence) { return ((uint64_t)source<<8)|sequence; }
static bool pending(const core_broadcast_record_t *r) { return r->ack_goal!=0; }
static uint8_t count_peers(const uint32_t peers[4]) {
    unsigned count=0;
    for (unsigned i=0;i<4;i++) {
        uint32_t bits=peers[i]; bits-=((bits>>1)&UINT32_C(0x55555555));
        bits=(bits&UINT32_C(0x33333333))+((bits>>2)&UINT32_C(0x33333333));
        bits=(bits+(bits>>4))&UINT32_C(0x0f0f0f0f);
        count+=(bits*UINT32_C(0x01010101))>>24;
    }
    return (uint8_t)count;
}
static void retry_unlink(core_broadcast_t *q, uint8_t slot) {
    core_broadcast_record_t *r=&q->records[slot];
    if (!r->retry_scheduled) return;
    if (r->retry_prev==NONE) q->retry_heads[r->retry_bucket]=r->retry_next;
    else q->records[r->retry_prev].retry_next=r->retry_next;
    if (r->retry_next!=NONE) q->records[r->retry_next].retry_prev=r->retry_prev;
    r->retry_scheduled=false; r->retry_prev=r->retry_next=NONE;
}
static void remove_slot(core_broadcast_t *q, uint8_t slot) {
    core_broadcast_record_t *r=&q->records[slot];
    retry_unlink(q,slot);
    if (r->expiry_prev==NONE) q->expiry_heads[r->expiry_bucket]=r->expiry_next;
    else q->records[r->expiry_prev].expiry_next=r->expiry_next;
    if (r->expiry_next!=NONE) q->records[r->expiry_next].expiry_prev=r->expiry_prev;
    core_index_remove(&q->transactions,q->keys[slot]);
    r->used=false; memset(r->pending,0,sizeof(r->pending));
    q->free_next[slot]=q->free_head; q->free_head=slot;
}
void core_broadcast_init(core_broadcast_t *q, uint32_t now) {
    memset(q,0,sizeof(*q)); q->now=now;
    memset(q->expiry_heads,NONE,sizeof(q->expiry_heads));
    memset(q->retry_heads,NONE,sizeof(q->retry_heads));
    core_index_init(&q->transactions,q->branches,q->keys,q->occupied,CORE_BROADCAST_CAPACITY);
    for (uint8_t i=0;i<CORE_BROADCAST_CAPACITY;i++) q->free_next[i]=i+1u<CORE_BROADCAST_CAPACITY?(uint8_t)(i+1u):NONE;
}
core_broadcast_result_t core_broadcast_admit(core_broadcast_t *q, uint16_t source,
    uint8_t sequence, uint8_t lifetime_ticks, uint8_t ack_ticks, uint8_t retries,
    const uint32_t peers[CORE_BROADCAST_WORDS], uint16_t *slot) {
    if (slot) *slot=CORE_INDEX_NONE;
    if (source>=0xfff8u || !lifetime_ticks || !ack_ticks || !peers) return CORE_BCAST_INVALID;
    uint16_t existing=core_index_find(&q->transactions,key(source,sequence));
    if (existing!=CORE_INDEX_NONE) { if (slot) *slot=existing; return CORE_BCAST_DUPLICATE; }
    if (q->free_head==NONE) return CORE_BCAST_FULL;
    uint8_t id=q->free_head;
    if (!core_index_insert(&q->transactions,key(source,sequence),id)) return CORE_BCAST_FULL;
    q->free_head=q->free_next[id];
    core_broadcast_record_t *r=&q->records[id];
    *r=(core_broadcast_record_t){.used=true,.waiting_confirmation=true,
        .expiry_prev=NONE,.retry_prev=NONE,.retry_next=NONE,
        .expiry_bucket=(uint8_t)((q->now+lifetime_ticks)&127u),.expiry_round=lifetime_ticks>128,
        .ack_ticks=ack_ticks,.retries=retries};
    memcpy(r->pending,peers,sizeof(r->pending));
    r->ack_goal=count_peers(peers);
    r->expiry_next=q->expiry_heads[r->expiry_bucket];
    if (r->expiry_next!=NONE) q->records[r->expiry_next].expiry_prev=id;
    q->expiry_heads[r->expiry_bucket]=id;
    if (slot) *slot=id;
    return CORE_BCAST_NEW;
}
bool core_broadcast_ack(core_broadcast_t *q, uint16_t source, uint8_t sequence, uint8_t peer) {
    if (peer>=CORE_BROADCAST_PEERS) return false;
    uint16_t slot=core_index_find(&q->transactions,key(source,sequence));
    if (slot==CORE_INDEX_NONE) return false;
    core_broadcast_record_t *r=&q->records[slot];
    uint32_t bit=UINT32_C(1)<<(peer&31);
    if (!(r->pending[peer>>5]&bit)) return false;
    r->pending[peer>>5]&=~bit;
    if (r->ack_goal) r->ack_goal--;
    if (!pending(r) && !r->waiting_confirmation) {
        r->completed=true; retry_unlink(q,(uint8_t)slot);
    }
    return true;
}
bool core_broadcast_confirm(core_broadcast_t *q, uint16_t source, uint8_t sequence) {
    uint16_t slot=core_index_find(&q->transactions,key(source,sequence));
    if (slot==CORE_INDEX_NONE) return false;
    core_broadcast_record_t *r=&q->records[slot];
    if (!r->waiting_confirmation) return false;
    r->waiting_confirmation=false;
    if (!pending(r)) { r->completed=true; return true; }
    r->retry_bucket=(uint8_t)((q->now+r->ack_ticks)&127u);
    r->retry_round=r->ack_ticks>128;
    r->retry_prev=NONE; r->retry_next=q->retry_heads[r->retry_bucket];
    if (r->retry_next!=NONE) q->records[r->retry_next].retry_prev=(uint8_t)slot;
    q->retry_heads[r->retry_bucket]=(uint8_t)slot; r->retry_scheduled=true;
    return true;
}
void core_broadcast_tick(core_broadcast_t *q, core_broadcast_events_t *events) {
    *events=(core_broadcast_events_t){0}; uint8_t bucket=(uint8_t)(++q->now&127u);
    /* Iteration is proportional to due events, never to the entire BTT. */
    uint8_t slot=q->expiry_heads[bucket];
    while (slot!=NONE) {
        core_broadcast_record_t *r=&q->records[slot]; uint8_t next=r->expiry_next;
        if (r->expiry_round) r->expiry_round=false;
        else { events->expired|=UINT64_C(1)<<slot; remove_slot(q,slot); }
        slot=next;
    }
    slot=q->retry_heads[bucket];
    while (slot!=NONE) {
        core_broadcast_record_t *r=&q->records[slot]; uint8_t next=r->retry_next;
        if (r->retry_round) { r->retry_round=false; slot=next; continue; }
        retry_unlink(q,slot);
        if (!pending(r) || !r->retries) {
            r->completed=true; events->complete|=UINT64_C(1)<<slot;
        } else {
            r->retries--; r->waiting_confirmation=true;
            events->retry|=UINT64_C(1)<<slot;
        }
        slot=next;
    }
}
void core_broadcast_limit_acks(core_broadcast_t *q, uint16_t source, uint8_t sequence, uint8_t limit) {
    uint16_t slot=core_index_find(&q->transactions,key(source,sequence));
    if (slot==CORE_INDEX_NONE) return;
    core_broadcast_record_t *r=&q->records[slot];
    if (r->ack_goal>limit) r->ack_goal=limit;
    if (!pending(r) && !r->waiting_confirmation) { r->completed=true; retry_unlink(q,(uint8_t)slot); }
}
void core_broadcast_forget_peer(core_broadcast_t *q, uint8_t peer) {
    if (peer>=CORE_BROADCAST_PEERS) return;
    /* Membership churn fans out to affected transactions; the per-frame
     * lookup/ACK path remains indexed. This is not a per-frame full scan. */
    for (uint16_t slot=0;slot<CORE_BROADCAST_CAPACITY;slot++) if (q->records[slot].used) {
        core_broadcast_record_t *r=&q->records[slot];
        r->pending[peer>>5]&=~(UINT32_C(1)<<(peer&31));
        uint8_t remaining=count_peers(r->pending);
        if (r->ack_goal>remaining) r->ack_goal=remaining;
        if (!pending(r) && !r->waiting_confirmation) { r->completed=true; retry_unlink(q,(uint8_t)slot); }
    }
}
bool core_broadcast_cancel(core_broadcast_t *q, uint16_t source, uint8_t sequence) {
    uint16_t slot=core_index_find(&q->transactions,key(source,sequence));
    if (slot==CORE_INDEX_NONE) return false;
    remove_slot(q,(uint8_t)slot); return true;
}
