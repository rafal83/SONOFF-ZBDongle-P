/* SPDX-License-Identifier: MIT
 * See nwk_route.h. Zigbee PRO references: 3.4.8 (Link Status), 3.6.3.4
 * (Route Record / source routing), 3.6.4.3 (neighbour table and costs).
 */
#include "nwk_route.h"

void nwk_routes_init(nwk_routes_t *t) { *t = (nwk_routes_t){0}; }

void nwk_routes_clear(nwk_routes_t *t) {
    uint8_t id = t->rreq_id;
    *t = (nwk_routes_t){0};
    t->rreq_id = id;
}

uint8_t nwk_lqi_cost(uint8_t lqi) {
    /* Monotonic mapping of the CC26xx LQI scale to the 1..7 link cost. */
    if (lqi >= 200) return 1;
    if (lqi >= 170) return 2;
    if (lqi >= 140) return 3;
    if (lqi >= 110) return 5;
    return 7;
}

static nwk_neighbor_t *find_neighbor(nwk_routes_t *t, uint16_t addr) {
    for (unsigned i = 0; i < NWK_NEIGHBORS; i++)
        if (t->neighbors[i].used && t->neighbors[i].addr == addr) return &t->neighbors[i];
    return 0;
}

const nwk_neighbor_t *nwk_neighbor(const nwk_routes_t *t, uint16_t addr) {
    return find_neighbor((nwk_routes_t *)t, addr);
}

void nwk_neighbor_heard(nwk_routes_t *t, uint16_t addr, uint8_t lqi) {
    nwk_neighbor_t *n = find_neighbor(t, addr);
    if (!n) return;   /* only routers that sent a link status are neighbours */
    /* Smoothed LQI: 3/4 old + 1/4 new. */
    n->lqi = (uint8_t)((3u * n->lqi + lqi) / 4u);
    n->in_cost = nwk_lqi_cost(n->lqi);
}

bool nwk_neighbor_link_status(nwk_routes_t *t, uint16_t addr, uint8_t lqi, uint16_t self,
                              const uint8_t *entries, size_t count, bool complete_list) {
    if (addr >= 0xFFF8u || addr == self) return false;
    nwk_neighbor_t *n = find_neighbor(t, addr);
    if (!n) {
        nwk_neighbor_t *victim = 0;
        for (unsigned i = 0; i < NWK_NEIGHBORS && !victim; i++) if (!t->neighbors[i].used) victim = &t->neighbors[i];
        if (!victim) {   /* table full: replace the worst link, if this one is better */
            victim = &t->neighbors[0];
            for (unsigned i = 1; i < NWK_NEIGHBORS; i++)
                if (t->neighbors[i].in_cost > victim->in_cost) victim = &t->neighbors[i];
            if (victim->in_cost <= nwk_lqi_cost(lqi)) return false;
        }
        *victim = (nwk_neighbor_t){.addr = addr, .lqi = lqi, .used = true};
        n = victim;
    } else {
        n->lqi = (uint8_t)((3u * n->lqi + lqi) / 4u);
    }
    n->in_cost = nwk_lqi_cost(n->lqi);
    n->age = 0;
    uint8_t before = n->out_cost, now = 0;
    bool listed = false;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *e = entries + 3u * i;
        if ((uint16_t)(e[0] | (e[1] << 8)) == self) { now = e[2] & 0x07u; listed = true; }   /* its incoming = our outgoing */
    }
    /* Only a complete list (single frame) proves we are absent from it. */
    if (listed || complete_list) n->out_cost = now;
    return complete_list && !listed && before != 0;   /* it knew us and forgot us: it restarted */
}

void nwk_neighbors_age(nwk_routes_t *t) {
    for (unsigned i = 0; i < NWK_NEIGHBORS; i++) {
        nwk_neighbor_t *n = &t->neighbors[i];
        if (!n->used) continue;
        if (++n->age > NWK_ROUTER_AGE_LIMIT) { n->used = false; n->out_cost = 0; }
    }
}

void nwk_neighbor_remove(nwk_routes_t *t, uint16_t addr) {
    nwk_neighbor_t *n = find_neighbor(t, addr);
    if (n) n->used = false;
}

size_t nwk_link_status_build(const nwk_routes_t *t, uint8_t *out, size_t cap) {
    unsigned next;
    return nwk_link_status_chunk(t, 0, 31u, out, cap, &next);
}

size_t nwk_link_status_chunk(const nwk_routes_t *t, unsigned start, unsigned max_entries, uint8_t *out, size_t cap,
                             unsigned *next) {
    *next = 0;
    if (cap < 2) return 0;
    uint16_t addrs[NWK_NEIGHBORS];
    uint8_t costs[NWK_NEIGHBORS];
    unsigned n = 0;
    for (unsigned i = 0; i < NWK_NEIGHBORS; i++) {
        if (!t->neighbors[i].used) continue;
        addrs[n] = t->neighbors[i].addr;
        costs[n] = (uint8_t)((t->neighbors[i].in_cost & 7u) | ((t->neighbors[i].out_cost & 7u) << 4));
        n++;
    }
    for (unsigned i = 1; i < n; i++) {   /* ascending addresses (spec requirement) */
        for (unsigned j = i; j > 0 && addrs[j - 1] > addrs[j]; j--) {
            uint16_t a = addrs[j]; addrs[j] = addrs[j - 1]; addrs[j - 1] = a;
            uint8_t c = costs[j]; costs[j] = costs[j - 1]; costs[j - 1] = c;
        }
    }
    if (start > n) start = n;
    unsigned take = n - start;
    if (max_entries > 31u) max_entries = 31u;
    if (take > max_entries) take = max_entries;
    if (2u + 3u * take > cap) take = (unsigned)((cap - 2u) / 3u);
    *next = start + take;
    out[0] = 0x08;                                     /* Link Status */
    out[1] = (uint8_t)(take | (start == 0 ? 0x20u : 0u) | (*next >= n ? 0x40u : 0u));   /* first / last frame */
    for (unsigned i = 0; i < take; i++) {
        out[2 + 3 * i] = (uint8_t)addrs[start + i];
        out[3 + 3 * i] = (uint8_t)(addrs[start + i] >> 8);
        out[4 + 3 * i] = costs[start + i];
    }
    return 2u + 3u * take;
}

void nwk_source_route_set(nwk_routes_t *t, uint16_t dst, const uint16_t *relays, uint8_t count) {
    if (count > NWK_ROUTE_MAX_RELAYS || dst >= 0xFFF8u) return;
    nwk_source_route_t *slot = 0, *oldest = 0;
    for (unsigned i = 0; i < NWK_SOURCE_ROUTES; i++) {
        nwk_source_route_t *s = &t->source[i];
        if (s->used && s->dst == dst) { slot = s; break; }
        if (!s->used && !slot) slot = s;
        if (s->used && (!oldest || t->now_ms - s->ms > t->now_ms - oldest->ms)) oldest = s;
    }
    if (!slot) slot = oldest;
    if (!slot) return;
    slot->used = true;
    slot->dst = dst;
    slot->count = count;
    for (unsigned i = 0; i < count; i++) slot->relays[i] = relays[i];
    slot->ms = t->now_ms;
}

const nwk_source_route_t *nwk_source_route(const nwk_routes_t *t, uint16_t dst) {
    for (unsigned i = 0; i < NWK_SOURCE_ROUTES; i++)
        if (t->source[i].used && t->source[i].dst == dst) return &t->source[i];
    return 0;
}

void nwk_table_route_set(nwk_routes_t *t, uint16_t dst, uint16_t next_hop) {
    if (dst >= 0xFFF8u || next_hop >= 0xFFF8u) return;
    nwk_table_route_t *slot = 0, *oldest = 0;
    for (unsigned i = 0; i < NWK_TABLE_ROUTES; i++) {
        nwk_table_route_t *r = &t->table[i];
        if (r->used && r->dst == dst) { slot = r; break; }
        if (!r->used && !slot) slot = r;
        if (r->used && (!oldest || t->now_ms - r->ms > t->now_ms - oldest->ms)) oldest = r;
    }
    if (!slot) slot = oldest;
    if (!slot) return;
    *slot = (nwk_table_route_t){.dst = dst, .next_hop = next_hop, .used = true, .ms = t->now_ms};
}

bool nwk_table_route(const nwk_routes_t *t, uint16_t dst, uint16_t *next_hop) {
    for (unsigned i = 0; i < NWK_TABLE_ROUTES; i++) {
        if (t->table[i].used && t->table[i].dst == dst) { *next_hop = t->table[i].next_hop; return true; }
    }
    return false;
}

void nwk_routes_forget(nwk_routes_t *t, uint16_t dst) {
    for (unsigned i = 0; i < NWK_SOURCE_ROUTES; i++) {
        nwk_source_route_t *s = &t->source[i];
        if (!s->used) continue;
        if (s->dst == dst) { s->used = false; continue; }
        for (unsigned k = 0; k < s->count; k++) if (s->relays[k] == dst) { s->used = false; break; }
    }
    for (unsigned i = 0; i < NWK_TABLE_ROUTES; i++) {
        nwk_table_route_t *r = &t->table[i];
        if (r->used && (r->dst == dst || r->next_hop == dst)) r->used = false;
    }
}

unsigned nwk_routes_age(nwk_routes_t *t, uint32_t now_ms) {
    unsigned n = 0;
    for (unsigned i = 0; i < NWK_SOURCE_ROUTES; i++) {
        if (t->source[i].used && now_ms - t->source[i].ms > NWK_SOURCE_ROUTE_MAX_MS) { t->source[i].used = false; n++; }
    }
    for (unsigned i = 0; i < NWK_TABLE_ROUTES; i++) {
        if (t->table[i].used && now_ms - t->table[i].ms > NWK_TABLE_ROUTE_MAX_MS) { t->table[i].used = false; n++; }
    }
    return n;
}

bool nwk_neighbor_tx_result(nwk_routes_t *t, uint16_t addr, bool ok) {
    nwk_neighbor_t *n = find_neighbor(t, addr);
    if (!n) return false;
    if (ok) { n->failures = 0; return false; }
    if (++n->failures < NWK_NEIGHBOR_MAX_FAILURES) return false;
    n->used = false;
    return true;
}

bool nwk_btt_seen(const nwk_routes_t *t, uint16_t src, uint8_t seq, uint32_t now_ms) {
    for (unsigned i = 0; i < NWK_BTT_SIZE; i++) {
        if (!t->btt[i].used || now_ms - t->btt[i].ms > NWK_BTT_MS) continue;
        if (t->btt[i].src == src && t->btt[i].seq == seq) return true;
    }
    return false;
}

bool nwk_btt_accept(nwk_routes_t *t, uint16_t src, uint8_t seq, uint32_t now_ms) {
    for (unsigned i = 0; i < NWK_BTT_SIZE; i++) {
        if (!t->btt[i].used || now_ms - t->btt[i].ms > NWK_BTT_MS) continue;
        if (t->btt[i].src == src && t->btt[i].seq == seq) return false;
    }
    unsigned i = t->btt_next++ % NWK_BTT_SIZE;
    t->btt[i].used = true;
    t->btt[i].src = src;
    t->btt[i].seq = seq;
    t->btt[i].ms = now_ms;
    return true;
}
