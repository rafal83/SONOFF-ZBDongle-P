/* SPDX-License-Identifier: MIT */
#include "core_index.h"
#include <string.h>
static bool leaf(uint16_t node) { return (node & CORE_INDEX_LEAF) != 0; }
static inline unsigned direction(uint64_t key, uint8_t bit) {
    if (bit < 32u) return (unsigned)(((uint32_t)(key >> 32u) >> (31u - bit)) & 1u);
    return (unsigned)(((uint32_t)key >> (63u - bit)) & 1u);
}
static bool occupied(const core_index_t *index, uint16_t slot) {
    return (index->occupied[slot >> 3] & (1u << (slot & 7))) != 0;
}
static void occupy(core_index_t *index, uint16_t slot, bool used) {
    uint8_t mask=(uint8_t)(1u << (slot & 7));
    if (used) index->occupied[slot >> 3] |= mask;
    else index->occupied[slot >> 3] &= (uint8_t)~mask;
}
bool core_index_init(core_index_t *index, core_index_branch_t *branches,
                     uint64_t *keys, uint8_t *used, uint16_t capacity) {
    if (!index || !branches || !keys || !used || !capacity || capacity >= CORE_INDEX_LEAF) return false;
    *index=(core_index_t){.branches=branches,.keys=keys,.occupied=used,
        .root=CORE_INDEX_NONE,.free_branch=capacity>1?0:CORE_INDEX_NONE,.capacity=capacity};
    memset(used,0,(capacity+7u)/8u);
    for (uint16_t i=0;i+1u<capacity;i++) {
        branches[i].child[0]=i+2u<capacity?(uint16_t)(i+1u):CORE_INDEX_NONE;
        branches[i].bit=UINT8_MAX;
    }
    return true;
}
uint16_t core_index_find(core_index_t *index, uint64_t key) {
    uint16_t node=index->root;
    if (node==CORE_INDEX_NONE) { index->last_steps=0; return CORE_INDEX_NONE; }
    uint8_t steps=0;
    while (!leaf(node)) {
        steps++;
        node=index->branches[node].child[direction(key,index->branches[node].bit)];
    }
    index->last_steps=steps;
    uint16_t slot=(uint16_t)(node & ~CORE_INDEX_LEAF);
    return index->keys[slot]==key?slot:CORE_INDEX_NONE;
}
bool core_index_insert(core_index_t *index, uint64_t key, uint16_t slot) {
    if (slot>=index->capacity || occupied(index,slot)) return false;
    uint16_t node=index->root;
    if (node==CORE_INDEX_NONE) {
        index->keys[slot]=key; occupy(index,slot,true);
        index->root=CORE_INDEX_LEAF|slot; index->count=1; return true;
    }
    while (!leaf(node)) node=index->branches[node].child[direction(key,index->branches[node].bit)];
    uint64_t diff=key ^ index->keys[node & ~CORE_INDEX_LEAF];
    if (!diff || index->free_branch==CORE_INDEX_NONE) return false;
    uint8_t bit=0;
    for (uint64_t mask=UINT64_C(1)<<63; !(mask & diff); mask>>=1) bit++;
    uint16_t *link=&index->root;
    while (!leaf(*link) && index->branches[*link].bit<bit)
        link=&index->branches[*link].child[direction(key,index->branches[*link].bit)];
    uint16_t added=index->free_branch;
    index->free_branch=index->branches[added].child[0];
    unsigned side=direction(key,bit);
    index->branches[added].bit=bit;
    index->branches[added].child[side]=CORE_INDEX_LEAF|slot;
    index->branches[added].child[1u-side]=*link;
    index->keys[slot]=key; occupy(index,slot,true); *link=added; index->count++;
    return true;
}
uint16_t core_index_remove(core_index_t *index, uint64_t key) {
    if (index->root==CORE_INDEX_NONE) return CORE_INDEX_NONE;
    uint16_t *link=&index->root, *parent_link=NULL, parent=CORE_INDEX_NONE;
    unsigned side=0;
    while (!leaf(*link)) {
        parent_link=link; parent=*link;
        side=direction(key,index->branches[parent].bit);
        link=&index->branches[parent].child[side];
    }
    uint16_t slot=(uint16_t)(*link & ~CORE_INDEX_LEAF);
    if (index->keys[slot]!=key) return CORE_INDEX_NONE;
    if (parent==CORE_INDEX_NONE) index->root=CORE_INDEX_NONE;
    else {
        *parent_link=index->branches[parent].child[1u-side];
        index->branches[parent].child[0]=index->free_branch;
        index->branches[parent].bit=UINT8_MAX;
        index->free_branch=parent;
    }
    occupy(index,slot,false); index->keys[slot]=0; index->count--;
    return slot;
}
