/* SPDX-License-Identifier: MIT */
#include "nwk_devices.h"

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
    core_index_init(&t->by_ext, t->ext_branches, t->ext_keys, t->ext_occ, NWK_MAX_DEVICES);
    core_index_init(&t->by_short, t->short_branches, t->short_keys, t->short_occ, NWK_MAX_DEVICES);
    for (uint16_t i = 0; i < NWK_MAX_DEVICES; i++)
        t->free_next[i] = i + 1u < NWK_MAX_DEVICES ? (uint16_t)(i + 1u) : NWK_DEVICE_NONE;
    t->rng = seed;
}

uint16_t nwk_devices_find_ext(nwk_devices_t *t, uint64_t ext) { return core_index_find(&t->by_ext, ext); }
uint16_t nwk_devices_find_short(nwk_devices_t *t, uint16_t a) { return core_index_find(&t->by_short, a); }

static bool usable(uint16_t a) { return a != 0x0000 && a < 0xFFF8u; }

static uint16_t insert(nwk_devices_t *t, uint64_t ext, uint16_t short_addr) {
    if (t->free_head == NWK_DEVICE_NONE) { t->full_rejections++; return NWK_DEVICE_NONE; }
    uint16_t slot = t->free_head;
    if (!core_index_insert(&t->by_ext, ext, slot)) return NWK_DEVICE_NONE;
    if (!core_index_insert(&t->by_short, short_addr, slot)) {
        (void)core_index_remove(&t->by_ext, ext);
        return NWK_DEVICE_NONE;
    }
    t->free_head = t->free_next[slot];
    t->dev[slot] = (nwk_device_t){.ext = ext, .short_addr = short_addr};
    t->count++;
    return slot;
}

uint16_t nwk_devices_draw_address(nwk_devices_t *t) {
    for (unsigned i = 0; i < NWK_ADDR_DRAWS; i++) {
        uint16_t candidate = (uint16_t)next_random(t);
        if (!usable(candidate)) continue;
        if (nwk_devices_find_short(t, candidate) != CORE_INDEX_NONE) { t->address_conflicts_avoided++; continue; }
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
            if (nwk_devices_find_short(t, candidate) != CORE_INDEX_NONE) { t->address_conflicts_avoided++; continue; }
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
    if (owner != CORE_INDEX_NONE && owner != slot) return false;   /* address conflict: caller resolves */
    if (slot == NWK_DEVICE_NONE) {
        slot = insert(t, ext, short_addr);
        if (slot == NWK_DEVICE_NONE) return false;
    } else if (t->dev[slot].short_addr != short_addr) {
        /* core_index stores the key in the value's slot: remove first. */
        uint16_t old = t->dev[slot].short_addr;
        (void)core_index_remove(&t->by_short, old);
        if (!core_index_insert(&t->by_short, short_addr, slot)) {
            (void)core_index_insert(&t->by_short, old, slot);
            return false;
        }
        t->dev[slot].short_addr = short_addr;
    }
    if (relationship != NWK_REL_NONE) t->dev[slot].relationship = relationship;
    t->dev[slot].authorized = true;
    return true;
}

bool nwk_devices_remove(nwk_devices_t *t, uint64_t ext) {
    uint16_t slot = core_index_remove(&t->by_ext, ext);
    if (slot == CORE_INDEX_NONE) return false;
    (void)core_index_remove(&t->by_short, t->dev[slot].short_addr);
    t->dev[slot] = (nwk_device_t){0};
    t->free_next[slot] = t->free_head;
    t->free_head = slot;
    t->count--;
    return true;
}

bool nwk_devices_restore_slot(nwk_devices_t *t, uint16_t slot, const nwk_device_t *d) {
    if (slot >= NWK_MAX_DEVICES || t->dev[slot].ext || !d->ext || !usable(d->short_addr) ||
        nwk_devices_find_ext(t, d->ext) != CORE_INDEX_NONE ||
        nwk_devices_find_short(t, d->short_addr) != CORE_INDEX_NONE) return false;
    if (!core_index_insert(&t->by_ext, d->ext, slot)) return false;
    if (!core_index_insert(&t->by_short, d->short_addr, slot)) {
        (void)core_index_remove(&t->by_ext, d->ext);
        return false;
    }
    t->dev[slot] = *d;
    t->count++;
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
