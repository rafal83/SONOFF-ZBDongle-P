/* SPDX-License-Identifier: MIT */
#include "nv_index.h"
#include <string.h>
typedef struct { uint16_t low, high, location; } index_slot;
_Static_assert(sizeof(index_slot) == 6, "NV directory RAM geometry changed");
_Static_assert((PORT_NV_INDEX_SLOTS & (PORT_NV_INDEX_SLOTS - 1)) == 0, "Power of two required");
static index_slot slots[PORT_NV_INDEX_SLOTS], stash[PORT_NV_INDEX_STASH];
static uint32_t seed;
static bool ready, blocked;
port_nv_index_stats_t port_nv_index_stats;
static uint32_t key_of(index_slot s) { return (uint32_t)s.low | ((uint32_t)s.high << 16); }
static unsigned position(uint32_t key, unsigned way) {
    uint32_t h = key ^ seed ^ (0x9e3779b9u * (way + 1u));
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h & (PORT_NV_INDEX_SLOTS - 1u);
}
void port_nv_index_invalidate(void) {
    port_nv_index_stats.generation++;
    ready = false; blocked = false;
}
bool port_nv_index_ready(void) { return ready; }
bool port_nv_index_blocked(void) { return blocked; }
void port_nv_index_barrier(void) { port_nv_index_stats.barriers++; port_nv_index_invalidate(); }
void port_nv_index_begin(void) {
    memset(slots, 255, sizeof(slots)); memset(stash, 255, sizeof(stash));
    seed = 0x6a09e667u ^ port_nv_index_stats.generation;
    ready = blocked = false; port_nv_index_stats.builds++;
}
void port_nv_index_finish(bool complete) { ready = complete && !blocked; blocked = !ready; }
unsigned port_nv_index_get(uint32_t key, uint16_t locations[PORT_NV_INDEX_CANDIDATES]) {
    unsigned count = 0;
    port_nv_index_stats.lookups++;
    for (unsigned i = 0; i < 4; i++) {
        index_slot s = slots[position(key, i)];
        if (s.location != UINT16_MAX && key_of(s) == key) {
            bool duplicate = false;
            for (unsigned j = 0; j < count; j++) if (locations[j] == s.location) duplicate = true;
            if (!duplicate) locations[count++] = s.location;
        }
    }
    for (unsigned i = 0; i < PORT_NV_INDEX_STASH; i++)
        if (stash[i].location != UINT16_MAX && key_of(stash[i]) == key) locations[count++] = stash[i].location;
    return count;
}
void port_nv_index_remove(uint32_t key, uint16_t location) {
    for (unsigned i = 0; i < 4 + PORT_NV_INDEX_STASH; i++) {
        index_slot *s = i < 4 ? &slots[position(key, i)] : &stash[i - 4];
        if (s->location == location && key_of(*s) == key) s->location = UINT16_MAX;
    }
}
bool port_nv_index_put(uint32_t key, uint16_t location, bool replace) {
    if (blocked || location >= 3u * 8192u) return false;
    index_slot item = {(uint16_t)key, (uint16_t)(key >> 16), location};
    for (unsigned i = 0; i < 4 + PORT_NV_INDEX_STASH; i++) {
        index_slot *s = i < 4 ? &slots[position(key, i)] : &stash[i - 4];
        if (s->location != UINT16_MAX && key_of(*s) == key) {
            if (replace) { *s = item; return true; }
            if (s->location == location) return true;
        }
    }
    /* Bounded mutation work. A failed relocation disables the entire index;
     * no incomplete dictionary is ever used to return NOTFOUND to NVOCMP.
     */
    for (unsigned kick = 0; kick < 128; kick++) {
        uint32_t current_key = key_of(item);
        for (unsigned way = 0; way < 4; way++) {
            index_slot *s = &slots[position(current_key, way)];
            if (s->location == UINT16_MAX) { *s = item; return true; }
        }
        index_slot *s = &slots[position(current_key, (kick + current_key) & 3u)];
        index_slot evicted = *s; *s = item; item = evicted;
    }
    for (unsigned i = 0; i < PORT_NV_INDEX_STASH; i++)
        if (stash[i].location == UINT16_MAX) { stash[i] = item; return true; }
    port_nv_index_stats.overflows++;
    ready = false; blocked = true;
    return false;
}
