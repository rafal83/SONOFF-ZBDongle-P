/* SPDX-License-Identifier: MIT */
#include "nwk_devices.h"
#include <string.h>

_Static_assert(NWK_MAX_DEVICES <= 255u, "slot numbers are stored on 8 bits");

/* ---- sorted slot arrays ------------------------------------------------ */
/* First position whose key is >= `key` (binary search, <= 8 steps). */
static unsigned lower_ext(const nwk_devices_t *t, uint64_t key) {
    unsigned lo = 0, hi = t->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2u;
        if (t->dev[t->by_ext[mid]].ext < key) lo = mid + 1u; else hi = mid;
    }
    return lo;
}
static unsigned lower_short(const nwk_devices_t *t, uint16_t key) {
    unsigned lo = 0, hi = t->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2u;
        if (t->dev[t->by_short[mid]].short_addr < key) lo = mid + 1u; else hi = mid;
    }
    return lo;
}
/* Insert `slot` (its keys already in dev[slot]) into one array holding `n` entries. */
static void array_insert(uint8_t *a, unsigned n, unsigned pos, uint16_t slot) {
    memmove(a + pos + 1u, a + pos, n - pos);
    a[pos] = (uint8_t)slot;
}
static void array_remove(uint8_t *a, unsigned n, unsigned pos) { memmove(a + pos, a + pos + 1u, n - pos - 1u); }
/* Position of `slot` in by_short (its current dev[].short_addr). */
static unsigned short_pos(const nwk_devices_t *t, uint16_t slot) {
    unsigned pos = lower_short(t, t->dev[slot].short_addr);
    while (pos < t->count && t->by_short[pos] != slot) pos++;   /* keys are unique: one step at most */
    return pos;
}
/* dev[slot] filled (ext, short_addr unused by others): index it. */
static void index_add(nwk_devices_t *t, uint16_t slot) {
    array_insert(t->by_ext, t->count, lower_ext(t, t->dev[slot].ext), slot);
    array_insert(t->by_short, t->count, lower_short(t, t->dev[slot].short_addr), slot);
    t->count++;
}
static void index_del(nwk_devices_t *t, uint16_t slot) {
    unsigned pe = lower_ext(t, t->dev[slot].ext), ps = short_pos(t, slot);
    array_remove(t->by_ext, t->count, pe);
    array_remove(t->by_short, t->count, ps);
    t->count--;
}

static uint32_t next_random(nwk_devices_t *t) {
    /* xorshift32: address spreading only, NOT a cryptographic source. The
     * MCU seeds it from the TRNG; keys never come from here. */
    uint32_t x = t->rng ? t->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return t->rng = x;
}

void nwk_devices_init(nwk_devices_t *t, uint32_t seed) {
    *t = (nwk_devices_t){0};
    for (uint16_t i = 0; i < NWK_MAX_DEVICES; i++)
        t->free_next[i] = i + 1u < NWK_MAX_DEVICES ? (uint16_t)(i + 1u) : NWK_DEVICE_NONE;
    t->rng = seed;
}

uint16_t nwk_devices_find_ext(nwk_devices_t *t, uint64_t ext) {
    unsigned pos = lower_ext(t, ext);
    return pos < t->count && t->dev[t->by_ext[pos]].ext == ext ? t->by_ext[pos] : NWK_DEVICE_NONE;
}
uint16_t nwk_devices_find_short(nwk_devices_t *t, uint16_t a) {
    unsigned pos = lower_short(t, a);
    return pos < t->count && t->dev[t->by_short[pos]].short_addr == a ? t->by_short[pos] : NWK_DEVICE_NONE;
}

static bool usable(uint16_t a) { return a != 0x0000 && a < 0xFFF8u; }

static uint16_t insert(nwk_devices_t *t, uint64_t ext, uint16_t short_addr) {
    if (t->free_head == NWK_DEVICE_NONE) { t->full_rejections++; return NWK_DEVICE_NONE; }
    uint16_t slot = t->free_head;
    t->free_head = t->free_next[slot];
    t->dev[slot] = (nwk_device_t){.ext = ext, .short_addr = short_addr};
    index_add(t, slot);
    return slot;
}

uint16_t nwk_devices_draw_address(nwk_devices_t *t) {
    for (unsigned i = 0; i < NWK_ADDR_DRAWS; i++) {
        uint16_t candidate = (uint16_t)next_random(t);
        if (!usable(candidate)) continue;
        if (nwk_devices_find_short(t, candidate) != NWK_DEVICE_NONE) { t->address_conflicts_avoided++; continue; }
        return candidate;
    }
    return 0;
}

uint8_t nwk_devices_associate(nwk_devices_t *t, uint64_t ext, uint8_t capability, bool permit,
                              uint32_t now_ms, uint16_t *short_out) {
    *short_out = 0xFFFF;
    uint16_t slot = nwk_devices_find_ext(t, ext);
    /* Closed network: refused even for a known IEEE, because a secure rejoin
     * does not go through MAC association. */
    if (!permit) return NWK_ASSOC_ACCESS_DENIED;
    if (slot == NWK_DEVICE_NONE) {
        if (t->free_head == NWK_DEVICE_NONE) { t->full_rejections++; return NWK_ASSOC_PAN_AT_CAPACITY; }
        uint16_t a = 0;
        for (unsigned i = 0; i < NWK_ADDR_DRAWS; i++) {
            uint16_t candidate = (uint16_t)next_random(t);
            if (!usable(candidate)) continue;
            if (nwk_devices_find_short(t, candidate) != NWK_DEVICE_NONE) { t->address_conflicts_avoided++; continue; }
            a = candidate;
            break;
        }
        if (!a) return NWK_ASSOC_PAN_AT_CAPACITY;   /* astronomically unlikely, still bounded */
        slot = insert(t, ext, a);
        if (slot == NWK_DEVICE_NONE) return NWK_ASSOC_PAN_AT_CAPACITY;
    }
    nwk_device_t *d = &t->dev[slot];
    d->capability = capability;
    d->relationship = NWK_REL_CHILD;
    d->authorized = false;            /* until the device proves it got the key */
    /* A (re)association restarts Trust Center authentication: the device
     * is back on its preconfigured key, the unique key no longer applies. */
    d->unique_key = false;
    d->aps_in_seen = false;
    d->last_seen_ms = now_ms;
    *short_out = d->short_addr;
    return NWK_ASSOC_SUCCESS;
}

bool nwk_devices_learn(nwk_devices_t *t, uint64_t ext, uint16_t short_addr, uint8_t relationship) {
    if (!usable(short_addr)) return false;
    uint16_t owner = nwk_devices_find_short(t, short_addr);
    uint16_t slot = nwk_devices_find_ext(t, ext);
    if (owner != NWK_DEVICE_NONE && owner != slot) return false;   /* address conflict: caller resolves */
    if (slot == NWK_DEVICE_NONE) {
        slot = insert(t, ext, short_addr);
        if (slot == NWK_DEVICE_NONE) return false;
    } else if (t->dev[slot].short_addr != short_addr) {
        /* re-sorted under its new address (the caller checked it is free) */
        array_remove(t->by_short, t->count, short_pos(t, slot));
        t->count--;   /* by_short holds one entry less while it is re-sorted */
        t->dev[slot].short_addr = short_addr;
        array_insert(t->by_short, t->count, lower_short(t, short_addr), slot);
        t->count++;
    }
    if (relationship != NWK_REL_NONE) t->dev[slot].relationship = relationship;
    t->dev[slot].authorized = true;
    return true;
}

bool nwk_devices_remove(nwk_devices_t *t, uint64_t ext) {
    uint16_t slot = nwk_devices_find_ext(t, ext);
    if (slot == NWK_DEVICE_NONE) return false;
    index_del(t, slot);
    t->dev[slot] = (nwk_device_t){0};
    t->free_next[slot] = t->free_head;
    t->free_head = slot;
    return true;
}

bool nwk_devices_restore_slot(nwk_devices_t *t, uint16_t slot, const nwk_device_t *d) {
    if (slot >= NWK_MAX_DEVICES || t->dev[slot].ext || !d->ext || !usable(d->short_addr) ||
        nwk_devices_find_ext(t, d->ext) != NWK_DEVICE_NONE ||
        nwk_devices_find_short(t, d->short_addr) != NWK_DEVICE_NONE || t->count >= NWK_MAX_DEVICES) return false;
    t->dev[slot] = *d;
    index_add(t, slot);
    return true;
}

void nwk_devices_rebuild_free(nwk_devices_t *t) {
    t->free_head = NWK_DEVICE_NONE;
    for (int i = (int)NWK_MAX_DEVICES - 1; i >= 0; i--) {
        if (t->dev[i].ext) continue;
        t->free_next[i] = t->free_head;
        t->free_head = (uint16_t)i;
    }
}
