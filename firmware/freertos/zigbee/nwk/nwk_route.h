/* SPDX-License-Identifier: MIT
 * NWK routing state of the coordinator (Zigbee PRO 3.6.3/3.6.4, concentrator
 * role as in Z-Stack): neighbour routers learnt from Link Status, source
 * routes learnt from Route Record (answers to our Many-to-One Route
 * Request), next-hop routes learnt from Route Reply, and the broadcast
 * transaction table that suppresses relayed duplicates.
 * Pure bookkeeping: no frames are sent from here. Nothing is persisted
 * (as in Z-Stack, routes are rebuilt by the periodic many-to-one request).
 */
#ifndef NWK_ROUTE_H
#define NWK_ROUTE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NWK_NEIGHBORS 32u
#define NWK_SOURCE_ROUTES 200u          /* one per device of the table (137 in production) */
#define NWK_ROUTE_MAX_RELAYS 10u
#define NWK_TABLE_ROUTES 32u
#define NWK_BTT_SIZE 32u
#define NWK_BTT_MS 9000u                 /* nwkBroadcastDeliveryTime */
#define NWK_ROUTER_AGE_LIMIT 3u          /* link status periods without news */

typedef struct {
    uint16_t addr;
    uint8_t in_cost, out_cost, age, lqi, failures;   /* failures: consecutive MAC failures */
    bool used;
} nwk_neighbor_t;

/* Field order chosen to avoid padding (28 bytes instead of 36: 1.6 KB over
 * the 200 entries). Replacement is least-recently-refreshed, by age of `ms`. */
typedef struct {
    uint32_t ms;                             /* last refresh time (also the LRU order) */
    uint16_t dst;
    uint16_t relays[NWK_ROUTE_MAX_RELAYS];   /* relays[0] is next to the destination */
    uint8_t count;
    bool used;
} nwk_source_route_t;

typedef struct {
    uint32_t ms;                             /* last refresh time (also the LRU order) */
    uint16_t dst, next_hop;
    bool used;
} nwk_table_route_t;

typedef struct {
    nwk_neighbor_t neighbors[NWK_NEIGHBORS];
    nwk_source_route_t source[NWK_SOURCE_ROUTES];
    nwk_table_route_t table[NWK_TABLE_ROUTES];
    struct { uint16_t src; uint8_t seq; uint32_t ms; bool used; } btt[NWK_BTT_SIZE];
    uint8_t btt_next;
    uint32_t now_ms;                         /* set by the owner before each update */
    uint8_t rreq_id;
} nwk_routes_t;

#define NWK_NEIGHBOR_MAX_FAILURES 3u
#define NWK_SOURCE_ROUTE_MAX_MS 360000u      /* 3 many-to-one periods without a Route Record */
#define NWK_TABLE_ROUTE_MAX_MS 300000u       /* nwkRouteExpiryTime-like */

void nwk_routes_init(nwk_routes_t *t);
void nwk_routes_clear(nwk_routes_t *t);
/* Link cost from a received LQI (1 good .. 7 poor). */
uint8_t nwk_lqi_cost(uint8_t lqi);

/* A frame was received from router `addr` (MAC source). */
void nwk_neighbor_heard(nwk_routes_t *t, uint16_t addr, uint8_t lqi);
/* Link Status from router `addr` (radius 1): `entries` = (address, status
 * byte) pairs; the entry naming `self` gives our outgoing cost. */
/* Returns true when a neighbour that listed us sends a COMPLETE list
 * (first and last frame) without us: it lost its tables (restart). */
bool nwk_neighbor_link_status(nwk_routes_t *t, uint16_t addr, uint8_t lqi, uint16_t self,
                              const uint8_t *entries, size_t count, bool complete_list);
/* Called once per link status period: ages neighbours and drops stale ones. */
void nwk_neighbors_age(nwk_routes_t *t);
const nwk_neighbor_t *nwk_neighbor(const nwk_routes_t *t, uint16_t addr);
void nwk_neighbor_remove(nwk_routes_t *t, uint16_t addr);
/* Link Status command payload (command id included), entries sorted. */
size_t nwk_link_status_build(const nwk_routes_t *t, uint8_t *out, size_t cap);
/* One frame of a Link Status split over several frames: entries
 * [start, start + max_entries) of the sorted list, with the first/last frame
 * flags; *next receives the next start (== count when this was the last). */
size_t nwk_link_status_chunk(const nwk_routes_t *t, unsigned start, unsigned max_entries, uint8_t *out, size_t cap,
                             unsigned *next);

/* Route Record received for `dst`: relays in arrival order (first = next to
 * the originator). An empty list means the originator is a neighbour. */
void nwk_source_route_set(nwk_routes_t *t, uint16_t dst, const uint16_t *relays, uint8_t count);
const nwk_source_route_t *nwk_source_route(const nwk_routes_t *t, uint16_t dst);
void nwk_table_route_set(nwk_routes_t *t, uint16_t dst, uint16_t next_hop);
bool nwk_table_route(const nwk_routes_t *t, uint16_t dst, uint16_t *next_hop);
/* Forget every route to `dst` (and every route through it as a next hop). */
void nwk_routes_forget(nwk_routes_t *t, uint16_t dst);

/* Drop routes not refreshed within their lifetime (returns how many). */
unsigned nwk_routes_age(nwk_routes_t *t, uint32_t now_ms);
/* Outcome of a unicast to neighbour `addr`: a success clears its failure
 * count; NWK_NEIGHBOR_MAX_FAILURES consecutive failures remove it (true). */
bool nwk_neighbor_tx_result(nwk_routes_t *t, uint16_t addr, bool ok);

/* Broadcast transaction table: true when (src, seq) is new (and records it). */
bool nwk_btt_accept(nwk_routes_t *t, uint16_t src, uint8_t seq, uint32_t now_ms);
#endif
