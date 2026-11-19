/* SPDX-License-Identifier: MIT */
#include "core_broadcast.h"
#include <assert.h>
#include <stdio.h>
static core_broadcast_t btt;
int main(void) {
    uint32_t peers[CORE_BROADCAST_WORDS]={1,0,0,UINT32_C(1)<<31};
    core_broadcast_init(&btt,UINT32_MAX-3u);
    uint16_t slot, same; core_broadcast_events_t e;
    assert(core_broadcast_admit(&btt,1,255,9,2,2,peers,&slot)==CORE_BCAST_NEW);
    assert(core_broadcast_admit(&btt,1,255,9,2,2,peers,&same)==CORE_BCAST_DUPLICATE && slot==same);
    assert(!core_broadcast_ack(&btt,1,255,1));
    assert(core_broadcast_ack(&btt,1,255,127));
    assert(!core_broadcast_ack(&btt,1,255,127));
    assert(core_broadcast_confirm(&btt,1,255)); assert(!core_broadcast_confirm(&btt,1,255));
    core_broadcast_tick(&btt,&e); assert(!e.retry && !e.expired);
    core_broadcast_tick(&btt,&e); assert(e.retry==(UINT64_C(1)<<slot));
    core_broadcast_tick(&btt,&e); assert(!e.retry);
    assert(core_broadcast_confirm(&btt,1,255));
    core_broadcast_tick(&btt,&e); assert(!e.retry);
    core_broadcast_tick(&btt,&e); assert(e.retry==(UINT64_C(1)<<slot));
    assert(core_broadcast_ack(&btt,1,255,0));
    assert(core_broadcast_confirm(&btt,1,255)); assert(btt.records[slot].completed);
    for (unsigned i=0;i<4;i++) core_broadcast_tick(&btt,&e);
    assert(e.expired==(UINT64_C(1)<<slot)); assert(!btt.transactions.count);
    assert(core_broadcast_admit(&btt,1,255,1,1,1,peers,&slot)==CORE_BCAST_NEW);
    assert(core_broadcast_confirm(&btt,1,255));
    core_broadcast_tick(&btt,&e); assert(e.expired && !e.retry && !e.complete);
    for (unsigned cycle=0;cycle<1000;cycle++) {
        for (unsigned i=0;i<CORE_BROADCAST_CAPACITY;i++)
            assert(core_broadcast_admit(&btt,(uint16_t)i,(uint8_t)cycle,255,2,0,peers,&slot)==CORE_BCAST_NEW);
        assert(core_broadcast_admit(&btt,100,0,1,1,0,peers,&slot)==CORE_BCAST_FULL);
        for (unsigned i=0;i<CORE_BROADCAST_CAPACITY;i++) assert(core_broadcast_confirm(&btt,(uint16_t)i,(uint8_t)cycle));
        core_broadcast_tick(&btt,&e); assert(!e.complete);
        core_broadcast_tick(&btt,&e); assert(e.complete==((UINT64_C(1)<<40)-1));
        for (unsigned i=0;i<CORE_BROADCAST_CAPACITY;i++) assert(core_broadcast_cancel(&btt,(uint16_t)i,(uint8_t)cycle));
        assert(!btt.transactions.count);
    }
    /* Two 128-position wheels cover the full native 1..255 tick range. */
    const unsigned deadlines[]={1,127,128,129,255};
    for (unsigned d=0;d<sizeof(deadlines)/sizeof(deadlines[0]);d++) {
        core_broadcast_init(&btt,UINT32_MAX-64);
        assert(core_broadcast_admit(&btt,1,1,(uint8_t)deadlines[d],1,0,peers,&slot)==CORE_BCAST_NEW);
        for (unsigned i=1;i<=deadlines[d];i++) {
            core_broadcast_tick(&btt,&e);
            assert((e.expired!=0)==(i==deadlines[d]));
        }
    }
    core_broadcast_init(&btt,0);
    assert(core_broadcast_admit(&btt,1,1,255,129,1,peers,&slot)==CORE_BCAST_NEW);
    assert(core_broadcast_confirm(&btt,1,1));
    for (unsigned i=1;i<=129;i++) { core_broadcast_tick(&btt,&e); assert((e.retry!=0)==(i==129)); }
    printf("Broadcast: source/sequence duplicates, unique ACKs through peer 127, retries, expiry/cancel, tick wrap and 1000 full/reuse cycles; %zu bytes\n",sizeof(btt));
}
