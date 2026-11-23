/* SPDX-License-Identifier: MIT
 * Coordinator device table and stochastic address assignment (Zigbee PRO,
 * spec 3.6.1.7). Two arrays of slot numbers, sorted by IEEE and by short
 * address, give every lookup in at most 8 comparisons (binary search over
 * 200 entries, deterministic); they hold no copy of the keys (the table has
 * them): 400 bytes instead of the 5.6 KB of two Patricia indexes with their
 * own 64-bit key copies. Insertion / removal move at most 200 bytes.
 * Address generation retries a bounded number of random draws.
 */
#ifndef ZB_NWK_DEVICES_H
#define ZB_NWK_DEVICES_H
#include <stdbool.h>
#include <stdint.h>

#define NWK_MAX_DEVICES 200u
#define NWK_ADDR_DRAWS 16u
#define NWK_DEVICE_NONE 0xFFFFu

/* MAC capability information (802.15.4 7.3.1.2). */
#define NWK_CAP_ALT_PAN_COORD 0x01u
#define NWK_CAP_FFD 0x02u
#define NWK_CAP_MAINS 0x04u
#define NWK_CAP_RX_ON_IDLE 0x08u
#define NWK_CAP_SECURITY 0x40u
#define NWK_CAP_ALLOC_ADDR 0x80u

enum { NWK_REL_NONE = 0, NWK_REL_CHILD = 1, NWK_REL_NEIGHBOR = 2 };
enum { NWK_FLOOR_NWK = 1, NWK_FLOOR_APS = 2 };
enum { NWK_ASSOC_SUCCESS = 0x00, NWK_ASSOC_PAN_AT_CAPACITY = 0x01, NWK_ASSOC_ACCESS_DENIED = 0x02 };

typedef struct {
    uint64_t ext;
    uint16_t short_addr;
    uint8_t capability, relationship;
    bool authorized;            /* set once the device proved it holds the NWK key */
    bool unique_key;            /* link_key holds a verified unique TC link key */
    bool aps_in_seen;           /* aps_in_counter is valid for the current link key */
    uint8_t link_key[16];
    uint8_t tclk_shift;         /* Z-Stack seed shift of link_key, 0xFF = explicit key */
    uint32_t aps_in_counter;    /* last authenticated incoming APS frame counter */
    uint32_t last_seen_ms;
    uint8_t lqi;                /* last link quality heard from it (one hop) */
    bool capability_guessed;    /* imported from a backup: capability not known yet */
    uint8_t ed_timeout;         /* child: End Device Timeout index requested + 1, 0 = none (default) */
    uint8_t parent_annce_frame; /* child: frame of our last Parent_annce listing it, 1-based; 0 = not listed
                                 * (RAM only, fits the padding) */
    uint8_t floor_pending;      /* incoming counter floor not made durable: NWK_FLOOR_* (RAM only) */
    bool aps_default_key;       /* secures its APS frames with the default TC link key, not with the
                                 * unique key a backup gave it (legacy device); stored */
} nwk_device_t;

typedef struct {
    nwk_device_t dev[NWK_MAX_DEVICES];
    /* slots of the `count` devices, sorted by dev[].ext and by dev[].short_addr */
    uint8_t by_ext[NWK_MAX_DEVICES], by_short[NWK_MAX_DEVICES];
    uint16_t free_next[NWK_MAX_DEVICES], free_head, count;
    uint32_t rng;
    uint32_t address_conflicts_avoided, full_rejections;
} nwk_devices_t;

void nwk_devices_init(nwk_devices_t *t, uint32_t seed);
uint16_t nwk_devices_find_ext(nwk_devices_t *t, uint64_t ext);
uint16_t nwk_devices_find_short(nwk_devices_t *t, uint16_t short_addr);
/* Association decision. A known IEEE keeps its short address (rejoin);
 * otherwise a fresh random address in 0x0001..0xFFF7 is drawn. */
uint8_t nwk_devices_associate(nwk_devices_t *t, uint64_t ext, uint8_t capability, bool permit,
                              uint32_t now_ms, uint16_t *short_out);
/* Record a known (ext, short) pair, e.g. from a device announce or a
 * restored backup; moves the short address if it changed. */
bool nwk_devices_learn(nwk_devices_t *t, uint64_t ext, uint16_t short_addr, uint8_t relationship);
bool nwk_devices_remove(nwk_devices_t *t, uint64_t ext);
/* Fresh random unused address (0 if none found in NWK_ADDR_DRAWS draws). */
uint16_t nwk_devices_draw_address(nwk_devices_t *t);
/* Persistence: put a stored entry back into its original slot (table must
 * not hold that slot or IEEE yet), then rebuild the free list once. */
bool nwk_devices_restore_slot(nwk_devices_t *t, uint16_t slot, const nwk_device_t *d);
void nwk_devices_rebuild_free(nwk_devices_t *t);
#endif
