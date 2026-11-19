/* SPDX-License-Identifier: MIT */
#include "core_indirect.h"
#include <string.h>
#define NONE UINT8_MAX
enum { FREE, QUEUED, IN_FLIGHT };
static bool child_busy(const core_indirect_t *q, uint16_t address) {
    for (unsigned i=0;i<CORE_INDIRECT_BROADCASTS;i++) {
        const core_indirect_broadcast_t *b=&q->broadcasts[i];
        if (b->used && q->records[b->slot].state==IN_FLIGHT && b->in_flight_address==address) return true;
    }
    return false;
}
static core_indirect_token_t token(const core_indirect_t *q, uint8_t slot) {
    return ((uint64_t)q->records[slot].generation<<8)|slot;
}
static bool valid(const core_indirect_t *q, core_indirect_token_t id) {
    uint8_t slot=(uint8_t)id;
    return slot<CORE_INDIRECT_CAPACITY && q->records[slot].state!=FREE &&
        (id>>8)==q->records[slot].generation;
}
void core_indirect_init(core_indirect_t *q) {
    memset(q,0,sizeof(*q));
    core_index_init(&q->addresses,q->branches,q->keys,q->occupied,CORE_INDIRECT_CAPACITY);
    for (uint8_t i=0;i<CORE_INDIRECT_CAPACITY;i++) {
        uint8_t next=i+1u<CORE_INDIRECT_CAPACITY?(uint8_t)(i+1u):NONE;
        q->records[i].free_next=next; q->children[i].free_next=next;
    }
}
static void detach(core_indirect_t *q, uint8_t slot) {
    core_indirect_record_t *r=&q->records[slot];
    core_indirect_child_t *c=&q->children[r->child];
    if (r->prev==NONE) c->head=r->next; else q->records[r->prev].next=r->next;
    if (r->next==NONE) c->tail=r->prev; else q->records[r->next].prev=r->prev;
    r->prev=r->next=NONE;
}
core_indirect_token_t core_indirect_enqueue(core_indirect_t *q, uint16_t address, void *payload) {
    /* Broadcast holding needs its own multi-child delivery adapter; never
     * pretend a broadcast packet belongs to one unicast destination. */
    if (!payload || address>=0xfff8u || q->free_record==NONE ||
        q->records[q->free_record].generation==UINT32_MAX) return CORE_INDIRECT_NONE;
    uint16_t child=core_index_find(&q->addresses,address);
    if (child!=CORE_INDEX_NONE && q->children[child].count>=CORE_INDIRECT_PER_CHILD) return CORE_INDIRECT_NONE;
    if (child==CORE_INDEX_NONE) {
        if (q->free_child==NONE) return CORE_INDIRECT_NONE;
        child=q->free_child;
        if (!core_index_insert(&q->addresses,address,child)) return CORE_INDIRECT_NONE;
        q->free_child=q->children[child].free_next;
        q->children[child]=(core_indirect_child_t){.address=address,.head=NONE,.tail=NONE,.in_flight=NONE};
    }
    uint8_t slot=q->free_record;
    q->free_record=q->records[slot].free_next;
    uint32_t generation=q->records[slot].generation+1u;
    core_indirect_child_t *c=&q->children[child];
    q->records[slot]=(core_indirect_record_t){.child=(uint8_t)child,.prev=c->tail,
        .next=NONE,.state=QUEUED,.generation=generation,.payload=payload};
    if (c->tail==NONE) c->head=slot; else q->records[c->tail].next=slot;
    c->tail=slot; c->count++; q->count++;
    return token(q,slot);
}
core_indirect_token_t core_indirect_poll(core_indirect_t *q, uint16_t address, void **payload) {
    if (payload) *payload=NULL;
    if (child_busy(q,address)) return CORE_INDIRECT_NONE;
    uint16_t child=core_index_find(&q->addresses,address);
    if (child==CORE_INDEX_NONE || q->children[child].head==NONE || q->children[child].in_flight!=NONE) return CORE_INDIRECT_NONE;
    uint8_t slot=q->children[child].head;
    detach(q,slot); q->records[slot].state=IN_FLIGHT;
    q->children[child].in_flight=slot;
    if (payload) *payload=q->records[slot].payload;
    /* The in-flight packet continues to occupy its global/child quota. */
    return token(q,slot);
}
bool core_indirect_retry(core_indirect_t *q, core_indirect_token_t id) {
    if (!valid(q,id)) return false;
    uint8_t slot=(uint8_t)id;
    if (q->records[slot].state!=IN_FLIGHT || q->records[slot].child>=CORE_INDIRECT_CAPACITY) return false;
    core_indirect_record_t *r=&q->records[slot];
    core_indirect_child_t *c=&q->children[r->child];
    c->in_flight=NONE;
    r->state=QUEUED; r->prev=NONE; r->next=c->head;
    if (c->head==NONE) c->tail=(uint8_t)slot; else q->records[c->head].prev=(uint8_t)slot;
    c->head=(uint8_t)slot; return true;
}
bool core_indirect_release(core_indirect_t *q, core_indirect_token_t id, void **payload) {
    if (payload) *payload=NULL;
    if (!valid(q,id)) return false;
    uint8_t slot=(uint8_t)id;
    core_indirect_record_t *r=&q->records[slot];
    bool broadcast=r->child>=CORE_INDIRECT_CAPACITY;
    core_indirect_child_t *c=broadcast?NULL:&q->children[r->child];
    if (broadcast) q->broadcasts[r->child-CORE_INDIRECT_CAPACITY].used=false;
    else if (r->state==QUEUED) detach(q,(uint8_t)slot);
    else c->in_flight=NONE;
    if (payload) *payload=r->payload;
    r->payload=NULL; r->state=FREE; r->free_next=q->free_record; q->free_record=(uint8_t)slot;
    q->count--;
    if (c) c->count--;
    if (c && !c->count) {
        uint8_t child=r->child;
        core_index_remove(&q->addresses,c->address);
        c->free_next=q->free_child; q->free_child=child;
    }
    return true;
}
uint8_t core_indirect_count(core_indirect_t *q, uint16_t address) {
    uint16_t child=core_index_find(&q->addresses,address);
    return child==CORE_INDEX_NONE?0:q->children[child].count;
}
core_indirect_token_t core_indirect_broadcast_enqueue(core_indirect_t *q, void *payload,
    const uint32_t recipients[CORE_INDIRECT_CHILD_WORDS]) {
    if (!payload || !recipients || q->free_record==NONE || q->broadcast_order==UINT64_MAX ||
        q->records[q->free_record].generation==UINT32_MAX) return CORE_INDIRECT_NONE;
    uint32_t any=0;
    for (unsigned i=0;i<CORE_INDIRECT_CHILD_WORDS;i++) any|=recipients[i];
    if (!any) return CORE_INDIRECT_NONE;
    unsigned b=0;
    while (b<CORE_INDIRECT_BROADCASTS && q->broadcasts[b].used) b++;
    if (b==CORE_INDIRECT_BROADCASTS) return CORE_INDIRECT_NONE;
    uint8_t slot=q->free_record; q->free_record=q->records[slot].free_next;
    uint32_t generation=q->records[slot].generation+1u;
    q->records[slot]=(core_indirect_record_t){.child=(uint8_t)(CORE_INDIRECT_CAPACITY+b),
        .state=QUEUED,.generation=generation,.payload=payload};
    q->broadcasts[b]=(core_indirect_broadcast_t){.used=true,.slot=slot,
        .order=q->broadcast_order++,.in_flight_peer=NONE};
    memcpy(q->broadcasts[b].pending,recipients,sizeof(q->broadcasts[b].pending));
    q->count++; return token(q,slot);
}
core_indirect_token_t core_indirect_broadcast_poll(core_indirect_t *q, uint16_t address, uint8_t peer, void **payload) {
    if (payload) *payload=NULL;
    if (peer>=32u*CORE_INDIRECT_CHILD_WORDS || address>=0xfff8u || child_busy(q,address)) return CORE_INDIRECT_NONE;
    uint16_t child=core_index_find(&q->addresses,address);
    if (child!=CORE_INDEX_NONE && q->children[child].in_flight!=NONE) return CORE_INDIRECT_NONE;
    unsigned selected=CORE_INDIRECT_BROADCASTS;
    for (unsigned i=0;i<CORE_INDIRECT_BROADCASTS;i++) {
        core_indirect_broadcast_t *b=&q->broadcasts[i];
        if (!b->used || q->records[b->slot].state!=QUEUED || !(b->pending[peer>>5]&(UINT32_C(1)<<(peer&31)))) continue;
        if (selected==CORE_INDIRECT_BROADCASTS || b->order<q->broadcasts[selected].order) selected=i;
    }
    if (selected==CORE_INDIRECT_BROADCASTS) return CORE_INDIRECT_NONE;
    core_indirect_broadcast_t *b=&q->broadcasts[selected];
    b->in_flight_address=address; b->in_flight_peer=peer;
    q->records[b->slot].state=IN_FLIGHT;
    if (payload) *payload=q->records[b->slot].payload;
    return token(q,b->slot);
}
bool core_indirect_broadcast_confirm(core_indirect_t *q, core_indirect_token_t id, bool delivered) {
    if (!valid(q,id)) return false;
    core_indirect_record_t *r=&q->records[(uint8_t)id];
    if (r->state!=IN_FLIGHT || r->child<CORE_INDIRECT_CAPACITY) return false;
    core_indirect_broadcast_t *b=&q->broadcasts[r->child-CORE_INDIRECT_CAPACITY];
    if (delivered) b->pending[b->in_flight_peer>>5]&=~(UINT32_C(1)<<(b->in_flight_peer&31));
    b->in_flight_peer=NONE; r->state=QUEUED; return true;
}
uint8_t core_indirect_broadcast_pending(core_indirect_t *q, core_indirect_token_t id) {
    if (!valid(q,id)) return 0;
    core_indirect_record_t *r=&q->records[(uint8_t)id];
    if (r->child<CORE_INDIRECT_CAPACITY) return 0;
    unsigned count=0;
    const core_indirect_broadcast_t *b=&q->broadcasts[r->child-CORE_INDIRECT_CAPACITY];
    for (unsigned i=0;i<CORE_INDIRECT_CHILD_WORDS;i++) {
        uint32_t bits=b->pending[i];
        /* Parallel population count, independent of the number of children. */
        bits-=((bits>>1)&UINT32_C(0x55555555));
        bits=(bits&UINT32_C(0x33333333))+((bits>>2)&UINT32_C(0x33333333));
        bits=(bits+(bits>>4))&UINT32_C(0x0f0f0f0f);
        count+=(bits*UINT32_C(0x01010101))>>24;
    }
    return (uint8_t)count;
}
void core_indirect_broadcast_forget_peer(core_indirect_t *q, uint8_t peer) {
    if (peer>=32u*CORE_INDIRECT_CHILD_WORDS) return;
    for (unsigned i=0;i<CORE_INDIRECT_BROADCASTS;i++) if (q->broadcasts[i].used)
        q->broadcasts[i].pending[peer>>5]&=~(UINT32_C(1)<<(peer&31));
}
