/* SPDX-License-Identifier: MIT */
#include "core_index.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CAP 200
static core_index_branch_t branches[CAP-1];
static uint64_t keys[CAP];
static uint8_t occupied[(CAP+7)/8];
static core_index_t index_map;
static uint64_t seed=UINT64_C(0x25620017112026);
static uint64_t random64(void) { seed^=seed<<13; seed^=seed>>7; seed^=seed<<17; return seed; }
int main(void) {
    assert(!core_index_init(NULL,branches,keys,occupied,CAP));
    assert(core_index_init(&index_map,branches,keys,occupied,CAP));
    assert(core_index_find(&index_map,0)==CORE_INDEX_NONE);
    assert(core_index_insert(&index_map,0,0));
    assert(core_index_insert(&index_map,UINT64_MAX,1));
    assert(!core_index_insert(&index_map,0,2));
    assert(!core_index_insert(&index_map,123,1));
    assert(!core_index_insert(&index_map,123,CAP));
    assert(core_index_remove(&index_map,UINT64_MAX)==1);
    assert(core_index_remove(&index_map,0)==0);
    uint64_t reference[CAP]={0}; bool present[CAP]={false};
    unsigned max_steps=0;
    for (unsigned cycle=0;cycle<100000;cycle++) {
        unsigned slot=(unsigned)(random64()%CAP);
        if (present[slot]) {
            assert(core_index_remove(&index_map,reference[slot])==slot);
            present[slot]=false;
        } else {
            uint64_t key=random64();
            if (core_index_find(&index_map,key)!=CORE_INDEX_NONE) continue;
            assert(core_index_insert(&index_map,key,(uint16_t)slot));
            reference[slot]=key; present[slot]=true;
        }
        for (unsigned i=0;i<CAP;i++) if (present[i]) {
            assert(core_index_find(&index_map,reference[i])==i);
            assert(index_map.last_steps<=64);
            if (index_map.last_steps>max_steps) max_steps=index_map.last_steps;
        }
        uint64_t absent=random64(); bool found=false;
        for (unsigned i=0;i<CAP;i++) if (present[i] && reference[i]==absent) found=true;
        if (!found) assert(core_index_find(&index_map,absent)==CORE_INDEX_NONE);
    }
    for (unsigned i=0;i<CAP;i++) if (present[i]) assert(core_index_remove(&index_map,reference[i])==i);
    for (unsigned i=0;i<CAP;i++) assert(core_index_insert(&index_map,UINT64_C(0x842e14fffe000000)+i,(uint16_t)i));
    for (unsigned i=0;i<CAP;i++) assert(core_index_remove(&index_map,UINT64_C(0x842e14fffe000000)+i)==i);
    /* Every differing bit and the one-entry geometry, including zero keys. */
    assert(core_index_init(&index_map,branches,keys,occupied,CAP));
    assert(core_index_insert(&index_map,0,0));
    for (unsigned i=0;i<64;i++) assert(core_index_insert(&index_map,UINT64_C(1)<<i,(uint16_t)(i+1)));
    for (unsigned i=0;i<64;i++) assert(core_index_find(&index_map,UINT64_C(1)<<i)==i+1);
    assert(core_index_init(&index_map,branches,keys,occupied,1));
    assert(core_index_insert(&index_map,0,0)); assert(!core_index_insert(&index_map,1,0));
    assert(core_index_remove(&index_map,0)==0);
    printf("Patricia: 100000 mutations, exhaustive live lookups, full/reuse/64 bit boundaries; max observed branches=%u; branch=%zu bytes\n",max_steps,sizeof(core_index_branch_t));
}
