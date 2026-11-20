/* SPDX-License-Identifier: MIT
 * Mesh at scale: the REAL coordinator (zb_coord) in the middle of 80
 * simulated routers on a 9x9 grid (4-neighbourhood, up to 8 hops).
 *
 * Router model (built only from the frame codecs, no coordinator code):
 *  - many-to-one tree from our Many-to-One Route Request (breadth-first
 *    flood over the live links);
 *  - a report = Route Record (relay list) then a data frame, hop by hop up
 *    the tree, the last hop securing the frame toward the coordinator;
 *  - source-routed frames relayed hop by hop; a relay that cannot reach the
 *    next hop sends a Network Status (source route failure) to us;
 *  - a frame without source route is forwarded along the routers' own
 *    shortest path (route discovery state);
 *  - Route Request for a router: Route Reply back along the reverse path;
 *  - Link Status from the coordinator's neighbours;
 *  - radio: per-attempt loss probability, 3 MAC retransmissions per hop,
 *    collision bursts (high loss for a while).
 * Phases: healthy network, 10 % loss, 35 % collision bursts, 8 routers
 * removed (incl. 2 coordinator neighbours) with route repair, address
 * changes, route expiry -> discovery -> Route Reply; then congestion (busy
 * channel: CCA failures), transmit queue saturation (bursts from the host
 * with traffic arriving meanwhile), broadcast storm, router restarts.
 */
#include "frt_test.h"
#include "zb_ccm_backend.h"
#include "zb_coord.h"

#define GRID 9
#define NODES (GRID * GRID)
#define CENTER (GRID / 2 * GRID + GRID / 2)
#define PAN 0x4D4D
#define COORD_EXT 0x00124B00AABBCCDDull
static const uint8_t KEY[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F};

typedef struct {
    uint64_t ext;
    uint16_t short_addr;
    bool alive;
    uint8_t mac_seq, nwk_seq;
    int parent;          /* many-to-one tree: next node toward the coordinator (CENTER = coordinator) */
    core_security_t sec;
} router_t;
static router_t node[NODES];
static zb_coord_t coord;
static unsigned loss_percent, busy_percent;
static unsigned confirms_by_status[256], confirms_total;
static unsigned delivered[NODES], sent_attempts, lost_attempts;
static unsigned status_reports, rrep_sent, rreq_seen, reports_ok;

static bool reserve(void *ctx, uint32_t u) { (void)ctx; (void)u; return true; }
static void on_aps(void *ctx, const nwk_header_t *n, const aps_header_t *a, const uint8_t *p, size_t l, uint8_t q) {
    (void)ctx; (void)n; (void)a; (void)p; (void)l; (void)q;   /* counted by stats.aps_data_up */
}
static void on_confirm(void *ctx, uint8_t handle, uint8_t status) {
    (void)ctx; (void)handle;
    confirms_by_status[status]++;
    confirms_total++;
}
static bool link_up(int a, int b) {
    if (a < 0 || b < 0 || !node[a].alive || !node[b].alive) return false;
    int ax = a % GRID, ay = a / GRID, bx = b % GRID, by = b / GRID;
    return (ax == bx && (ay - by == 1 || by - ay == 1)) || (ay == by && (ax - bx == 1 || bx - ax == 1));
}
static bool attempt(void) { sent_attempts++; if (frt_rand() % 100 < loss_percent) { lost_attempts++; return false; } return true; }
static bool hop(int a, int b) {   /* one hop with 3 MAC retransmissions */
    if (!link_up(a, b)) return false;
    for (unsigned i = 0; i < 4; i++) if (attempt()) return true;
    return false;
}
static int by_short(uint16_t s) { for (int i = 0; i < NODES; i++) if (node[i].short_addr == s && i != CENTER) return i; return -1; }

/* Breadth-first distances/parents from `root` over live links. */
static int bfs_parent[NODES];
static void bfs(int root) {
    int queue[NODES], head = 0, tail = 0;
    for (int i = 0; i < NODES; i++) bfs_parent[i] = -2;
    bfs_parent[root] = -1;
    queue[tail++] = root;
    while (head < tail) {
        int u = queue[head++];
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (unsigned k = 0; k < 4; k++) {
            int x = u % GRID + dx[k], y = u / GRID + dy[k];
            if (x < 0 || y < 0 || x >= GRID || y >= GRID) continue;
            int v = y * GRID + x;
            if (bfs_parent[v] != -2 || !link_up(u, v)) continue;
            bfs_parent[v] = u;
            queue[tail++] = v;
        }
    }
}
static void many_to_one_tree(void) {   /* flood of our MTORR */
    bfs(CENTER);
    for (int i = 0; i < NODES; i++) node[i].parent = i == CENTER ? -1 : bfs_parent[i];
}

/* `last` hands a NWK frame (header h, plaintext payload) to the coordinator. */
static void deliver_to_coord(int last, nwk_header_t *h, const uint8_t *payload, size_t len, bool broadcast) {
    uint8_t nwk[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(h, nwk, sizeof nwk);
    CHECK(hl);
    memcpy(nwk + hl, payload, len);
    uint8_t local[8];
    for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(node[last].ext >> (8 * i));
    size_t total;
    CHECK(core_security_seal(&node[last].sec, 0, local, nwk, hl, len, sizeof nwk, &total));
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.seq = node[last].mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = broadcast ? 0xFFFF : 0x0000; f.ack_request = !broadcast;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = node[last].short_addr;
    uint8_t mpdu[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&f, nwk, total, mpdu, sizeof mpdu);
    CHECK(n);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
}

/* Climb the many-to-one tree from `from`; returns the last hop or -1. */
static int climb(int from, uint16_t *relays, uint8_t *count) {
    int cur = from;
    *count = 0;
    while (node[cur].parent != CENTER) {
        int next = node[cur].parent;
        if (next < 0 || !hop(cur, next)) return -1;
        if (relays) relays[(*count)++] = node[next].short_addr;
        cur = next;
        if (*count > NWK_ROUTE_MAX_RELAYS) return -1;
    }
    return hop(cur, CENTER) ? cur : -1;
}

/* A router reports (Route Record then data) through the tree. */
static bool report(int i) {
    if (!node[i].alive || node[i].parent < 0) return false;
    uint16_t relays[NWK_ROUTE_MAX_RELAYS + 1];
    uint8_t count;
    int last = climb(i, relays, &count);
    if (last < 0) return false;
    uint8_t rr[2 + 2 * NWK_ROUTE_MAX_RELAYS] = {0x05, count};
    for (unsigned k = 0; k < count; k++) { rr[2 + 2 * k] = (uint8_t)relays[k]; rr[3 + 2 * k] = (uint8_t)(relays[k] >> 8); }
    nwk_header_t h = {0};
    h.type = NWK_TYPE_COMMAND; h.src = node[i].short_addr; h.dst = 0x0000; h.radius = 30; h.seq = node[i].nwk_seq++;
    deliver_to_coord(last, &h, rr, 2u + 2u * count, false);
    const uint8_t aps[8 + 3] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, (uint8_t)node[i].nwk_seq, 0x18, 0x0A, 0x00};
    h.type = NWK_TYPE_DATA; h.seq = node[i].nwk_seq++;
    deliver_to_coord(last, &h, aps, sizeof aps, false);
    reports_ok++;
    return true;
}

/* A router tells us its route to `about` failed. */
static void network_status_from(int f, uint16_t about) {
    if (f < 0 || node[f].parent < 0) return;
    int last = f;
    if (node[f].parent != CENTER) last = climb(f, 0, &(uint8_t){0});
    else if (!hop(f, CENTER)) last = -1;
    if (last < 0) return;
    const uint8_t ns[4] = {0x03, 0x0B, (uint8_t)about, (uint8_t)(about >> 8)};
    nwk_header_t h = {0};
    h.type = NWK_TYPE_COMMAND; h.src = node[f].short_addr; h.dst = 0x0000; h.radius = 30; h.seq = node[f].nwk_seq++;
    deliver_to_coord(last, &h, ns, sizeof ns, false);
    status_reports++;
}

/* ---- the coordinator's transmissions ------------------------------- */
static uint8_t q[256][MAC_MAX_MPDU];
static uint8_t qlen[256];
static unsigned qhead, qtail;
static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx; (void)ack;
    memcpy(q[qtail % 256], mpdu, len);
    qlen[qtail % 256] = len;
    qtail++;
    return 0;
}
static void sim_match(void *ctx, bool e, uint64_t a, bool p) { (void)ctx; (void)e; (void)a; (void)p; }

static void forward_from(int at, const nwk_header_t *h) {
    int dst = by_short(h->dst);
    if (dst < 0) return;
    int cur = at;
    if (h->source_route) {
        for (int idx = (int)h->relay_index; idx > 0; idx--) {
            int next = by_short(h->relays[idx - 1]);
            if (!hop(cur, next)) { network_status_from(cur, h->dst); return; }
            cur = next;
        }
        if (cur != dst && !hop(cur, dst)) { network_status_from(cur, h->dst); return; }
    } else if (cur != dst) {
        bfs(dst);   /* the routers' own route toward dst */
        while (cur != dst) {
            int next = bfs_parent[cur];
            if (next < 0 || !hop(cur, next)) { network_status_from(cur, h->dst); return; }
            cur = next;
        }
    }
    delivered[dst]++;
}

static void pump(void) {
    while (qhead < qtail) {
        uint8_t mpdu[MAC_MAX_MPDU];
        uint8_t len = qlen[qhead % 256];
        memcpy(mpdu, q[qhead % 256], len);
        qhead++;
        mac_frame_t f;
        CHECK(mac_frame_parse(mpdu, len, &f) == MAC_OK);
        nwk_header_t h;
        bool nwk_ok = f.type == MAC_TYPE_DATA && nwk_parse(f.payload, f.payload_len, &h) == NWK_OK;
        if (f.dst_short == 0xFFFF) {
            zb_coord_tx_done(&coord, MAC_TX_OK, false);
            if (!nwk_ok || h.type != NWK_TYPE_COMMAND || h.src != 0x0000) continue;
            /* Our broadcast commands: decrypt to see which one. */
            uint8_t frame[MAC_MAX_MPDU];
            memcpy(frame, f.payload, f.payload_len);
            size_t plen;
            int any = by_short(0x1000);
            if (!core_security_open_authenticated_peer(&node[any].sec, frame, f.payload_len, h.header_len, &plen)) continue;
            const uint8_t *p = frame + h.header_len + 14;
            if (p[0] == 0x01 && (p[1] & 0x18)) many_to_one_tree();            /* MTORR */
            else if (p[0] == 0x01) {                                         /* route discovery */
                rreq_seen++;
                int target = by_short((uint16_t)(p[3] | (p[4] << 8)));
                if (target < 0 || !node[target].alive) continue;
                bfs(CENTER);
                if (bfs_parent[target] == -2) continue;
                int last = target;
                while (bfs_parent[last] != CENTER) last = bfs_parent[last];
                const uint8_t rrep[8] = {0x02, 0x00, p[2], 0x00, 0x00, p[3], p[4], 3};
                nwk_header_t rh = {0};
                rh.type = NWK_TYPE_COMMAND; rh.src = node[last].short_addr; rh.dst = 0x0000; rh.radius = 1;
                rh.seq = node[last].nwk_seq++;
                deliver_to_coord(last, &rh, rrep, sizeof rrep, false);
                rrep_sent++;
            }
            continue;
        }
        int first = by_short(f.dst_short);
        if (busy_percent && frt_rand() % 100 < busy_percent) {   /* CSMA gave up: channel busy */
            zb_coord_tx_done(&coord, MAC_TX_BUSY, false);
            continue;
        }
        bool acked = first >= 0 && link_up(CENTER, first) && attempt();
        zb_coord_tx_done(&coord, acked ? MAC_TX_OK : MAC_TX_NO_ACK, false);   /* the MAC retries itself */
        if (acked && nwk_ok) forward_from(first, &h);
    }
}

static void link_status_round(void) {
    for (int i = 0; i < NODES; i++) {
        if (i == CENTER || !link_up(i, CENTER)) continue;
        const uint8_t ls[5] = {0x08, 0x61, 0x00, 0x00, 0x11};
        nwk_header_t h = {0};
        h.type = NWK_TYPE_COMMAND; h.src = node[i].short_addr; h.dst = 0xFFFC; h.radius = 1; h.seq = node[i].nwk_seq++;
        deliver_to_coord(i, &h, ls, sizeof ls, true);
    }
}

static uint32_t now = 1, next_link_status = 15000;
static void advance(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 500) {
        now += 500;
        if (now >= next_link_status) { next_link_status = now + 15000; link_status_round(); }   /* routers' period */
        zb_coord_tick(&coord, now);
        pump();
    }
}
static const uint8_t ZCL[3] = {0x01, 0x22, 0x02};

/* Send one frame to every live, connected router, with up to `tries`
 * attempts (a failed attempt is followed by a report, as a device does). */
static unsigned send_all(unsigned tries, unsigned *first_try) {
    unsigned ok = 0, first = 0;
    bfs(CENTER);
    int reach[NODES];
    for (int i = 0; i < NODES; i++) reach[i] = bfs_parent[i];
    for (int i = 0; i < NODES; i++) {
        if (i == CENTER || !node[i].alive || reach[i] == -2) continue;
        for (unsigned t = 0; t < tries; t++) {
            unsigned before = delivered[i];
            uint8_t st = zb_coord_send_aps(&coord, node[i].short_addr, 1, 1, 6, 0x0104, ZCL, 3, false);
            pump();
            if (delivered[i] > before) { ok++; if (t == 0) first++; break; }
            if (st == ZB_SEND_NO_ROUTE) { advance(500); continue; }   /* discovery in flight */
            advance(11000);                                           /* early MTORR gap */
            (void)report(i);
        }
    }
    if (first_try) *first_try = first;
    return ok;
}
static unsigned live_connected(void) {
    bfs(CENTER);
    unsigned n = 0;
    for (int i = 0; i < NODES; i++) if (i != CENTER && node[i].alive && bfs_parent[i] != -2) n++;
    return n;
}

int main(void) {
    static const mac_radio_ops_t RADIO = {sim_tx, sim_match, 0};
    static const zb_coord_upper_t UPPER = {.reserve_nwk_counter = reserve,   /* durable reservation (RAM here) */
                                           .send_confirm = on_confirm, .aps_data = on_aps};
    zb_coord_init(&coord, &RADIO, &UPPER, COORD_EXT, 0x5EEDu, 1);
    const uint8_t epid[8] = {1, 1, 2, 3, 5, 8, 13, 21};
    zb_coord_form(&coord, PAN, 20, epid, KEY, 0);
    for (int i = 0; i < NODES; i++) {
        node[i].ext = 0xA4C1380000100000ull + (uint64_t)i;
        node[i].short_addr = i == CENTER ? 0x0000 : (uint16_t)(0x1000 + i);
        node[i].alive = true;
        core_security_init(&node[i].sec, 1, zb_ccm_backend_sw, reserve, 0);
        CHECK(core_security_set_key(&node[i].sec, 0, 0, KEY));
        if (i != CENTER)
            CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, node[i].ext, node[i].short_addr, false, 0, 0xFF));
    }
    CHECK(coord.devices.count == NODES - 1);

    /* ---- 1. healthy network ----------------------------------------- */
    link_status_round();
    advance(4000);                         /* link status + many-to-one request */
    CHECK(coord.stats.mtorr_tx >= 1);
    unsigned neigh = 0;
    for (unsigned i = 0; i < NWK_NEIGHBORS; i++) neigh += coord.routes.neighbors[i].used;
    CHECK(neigh == 4);
    for (int i = 0; i < NODES; i++) if (i != CENTER) CHECK(report(i));
    unsigned with_route = 0, max_relays = 0;
    for (int i = 0; i < NODES; i++) {
        if (i == CENTER) continue;
        const nwk_source_route_t *sr = nwk_source_route(&coord.routes, node[i].short_addr);
        if (sr) { with_route++; if (sr->count > max_relays) max_relays = sr->count; }
    }
    CHECK(with_route == NODES - 1 && max_relays == 7);   /* corner: 8 hops = 7 relays */
    unsigned first;
    CHECK(send_all(1, &first) == NODES - 1 && first == NODES - 1);
    printf("mesh_scale: healthy: %u routers, %u reached at the first attempt (max %u relays)\n", NODES - 1, first,
           max_relays);

    /* ---- 2. 10 % loss per attempt ----------------------------------- */
    loss_percent = 10;
    unsigned ok = send_all(4, &first);
    CHECK(ok == NODES - 1);
    CHECK(first >= (NODES - 1) * 9 / 10);
    printf("mesh_scale: 10%% loss: %u/%u delivered, %u at the first attempt (%u/%u attempts lost)\n",
           ok, NODES - 1, first, lost_attempts, sent_attempts);

    /* ---- 3. collision bursts (35 % loss) ---------------------------- */
    loss_percent = 35;
    ok = send_all(6, &first);
    CHECK(ok >= (NODES - 1) * 95 / 100);
    printf("mesh_scale: 35%% collision bursts: %u/%u delivered, %u at the first attempt\n", ok, NODES - 1, first);
    loss_percent = 0;
    for (int i = 0; i < NODES; i++) if (i != CENTER) (void)report(i);   /* settle */

    /* ---- 4. topology change: 8 routers removed ---------------------- */
    static const int killed[8] = {CENTER - 1, CENTER + GRID, CENTER - 2 * GRID - 1, CENTER - 2 * GRID,
                                  CENTER - 2 * GRID + 1, CENTER + 2, CENTER + 3, CENTER + 3 * GRID};
    for (unsigned k = 0; k < 8; k++) node[killed[k]].alive = false;
    uint32_t ns_before = coord.stats.network_status_rx, rf_before = coord.stats.route_failures;
    /* First pass with the stale routes: failures are reported and repaired. */
    unsigned stale_ok = send_all(1, 0);
    CHECK(coord.stats.network_status_rx > ns_before || coord.stats.route_failures > rf_before);
    CHECK(coord.stats.early_mtorr >= 1);
    /* Removed coordinator neighbours: dropped (MAC failures or link status ageing). */
    advance(4 * 15000);
    CHECK(!nwk_neighbor(&coord.routes, node[CENTER - 1].short_addr));
    CHECK(!nwk_neighbor(&coord.routes, node[CENTER + GRID].short_addr));
    link_status_round();
    advance(1000);
    for (int i = 0; i < NODES; i++) if (i != CENTER) (void)report(i);
    unsigned live = live_connected();
    ok = send_all(3, &first);
    CHECK(ok == live);
    printf("mesh_scale: 8 routers removed: %u/%u reached with stale routes, %u/%u after repair "
           "(%u network status, %u MAC route failures, %u early MTORR)\n",
           stale_ok, live, ok, live, coord.stats.network_status_rx - ns_before,
           coord.stats.route_failures - rf_before, coord.stats.early_mtorr);

    /* ---- 5. address changes ----------------------------------------- */
    for (int k = 0; k < 5; k++) {
        int i = k * 7 + 3;
        if (i == CENTER || !node[i].alive) continue;
        uint16_t old = node[i].short_addr;
        node[i].short_addr = (uint16_t)(0x6000 + k);
        /* Relayed announce of the new address. */
        uint8_t relays[1];
        (void)relays;
        int last = node[i].parent == CENTER ? i : climb(i, 0, &(uint8_t){0});
        if (last < 0) continue;
        uint8_t aps[8 + 12] = {0x08, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, (uint8_t)(0x40 + k)};
        aps[9] = (uint8_t)node[i].short_addr; aps[10] = (uint8_t)(node[i].short_addr >> 8);
        for (unsigned b = 0; b < 8; b++) aps[11 + b] = (uint8_t)(node[i].ext >> (8 * b));
        aps[19] = 0x8E;
        nwk_header_t h = {0};
        h.type = NWK_TYPE_DATA; h.src = node[i].short_addr; h.dst = 0xFFFD; h.radius = 30; h.seq = node[i].nwk_seq++;
        deliver_to_coord(last, &h, aps, sizeof aps, true);
        CHECK(nwk_devices_find_short(&coord.devices, old) == NWK_DEVICE_NONE);
        CHECK(nwk_devices_find_short(&coord.devices, node[i].short_addr) != NWK_DEVICE_NONE);
        CHECK(!nwk_source_route(&coord.routes, old));
        CHECK(report(i));
        unsigned before = delivered[i];
        CHECK(zb_coord_send_aps(&coord, node[i].short_addr, 1, 1, 6, 0x0104, ZCL, 3, false) == ZB_SEND_OK);
        pump();
        CHECK(delivered[i] == before + 1);
        /* The old address is gone everywhere (routes and neighbour table). */
        unsigned no_route = confirms_by_status[0xCD];
        CHECK(zb_coord_send_aps(&coord, old, 1, 1, 6, 0x0104, ZCL, 3, false) == ZB_SEND_OK);   /* held */
        advance(ZB_DISC_WAIT_MS + 1000u);
        CHECK(confirms_by_status[0xCD] == no_route + 1);   /* then NWK_NO_ROUTE to the host */
    }

    /* ---- 6. route expiry, discovery, Route Reply --------------------- */
    advance(NWK_SOURCE_ROUTE_MAX_MS + 30000u);   /* no Route Record meanwhile */
    CHECK(coord.stats.routes_expired > 0);
    int far = 0;   /* a corner router, far from us */
    CHECK(node[far].alive && !nwk_source_route(&coord.routes, node[far].short_addr));
    unsigned before = delivered[far];
    uint32_t held = coord.stats.disc_sent;
    CHECK(zb_coord_send_aps(&coord, node[far].short_addr, 1, 1, 6, 0x0104, ZCL, 3, false) == ZB_SEND_OK);
    pump();                                       /* the Route Request is answered */
    CHECK(rreq_seen > 0 && rrep_sent > 0);
    uint16_t nh;
    CHECK(nwk_table_route(&coord.routes, node[far].short_addr, &nh));
    CHECK(coord.stats.disc_sent == held + 1 && delivered[far] == before + 1);   /* held frame delivered */
    before = delivered[far];
    CHECK(zb_coord_send_aps(&coord, node[far].short_addr, 1, 1, 6, 0x0104, ZCL, 3, false) == ZB_SEND_OK);
    pump();
    CHECK(delivered[far] == before + 1);
    printf("mesh_scale: expiry: %u routes expired, discovery answered (%u RREQ seen, %u RREP)\n",
           coord.stats.routes_expired, rreq_seen, rrep_sent);

    /* ---- 7. congestion: busy channel (40 % of the coordinator's frames
     *      fail CSMA). Every failure reaches the host (0xE1), nothing is
     *      treated as a broken link: no neighbour dropped, no route
     *      forgotten, no early MTORR added to the busy channel. ---------- */
    for (int i = 0; i < NODES; i++) if (i != CENTER) (void)report(i);
    {
        uint32_t rf = coord.stats.route_failures, nd = coord.stats.neighbors_dropped, em = coord.stats.early_mtorr;
        unsigned cca = confirms_by_status[MAC_STATUS_CHANNEL_ACCESS_FAILURE];
        busy_percent = 40;
        live = live_connected();
        ok = send_all(10, &first);
        busy_percent = 0;
        CHECK(ok == live);
        CHECK(coord.stats.route_failures == rf && coord.stats.neighbors_dropped == nd && coord.stats.early_mtorr == em);
        CHECK(confirms_by_status[MAC_STATUS_CHANNEL_ACCESS_FAILURE] > cca);
        CHECK(coord.stats.tx_not_link_failures > 0);
        printf("mesh_scale: busy channel 40%%: %u/%u delivered (%u at the first attempt), %u CCA failures "
               "reported, routes kept\n", ok, live, first, confirms_by_status[MAC_STATUS_CHANNEL_ACCESS_FAILURE] - cca);
    }

    /* ---- 8. transmit queue saturation: the host sends to every router at
     *      once while reports keep arriving. Refusals are immediate and
     *      explicit, every accepted frame is confirmed exactly once and
     *      delivered, nothing leaks, the refused ones go through after. -- */
    for (int i = 0; i < NODES; i++) if (i != CENTER) (void)report(i);
    {
        bool accepted_to[NODES] = {0};
        unsigned accepted = 0, refused = 0, before[NODES];
        unsigned conf = confirms_total;
        for (int i = 0; i < NODES; i++) before[i] = delivered[i];
        bfs(CENTER);
        int reach[NODES];
        for (int i = 0; i < NODES; i++) reach[i] = bfs_parent[i];
        unsigned sent_n = 0;
        for (int i = 0; i < NODES; i++) {
            if (i == CENTER || !node[i].alive || reach[i] == -2) continue;
            uint8_t st = zb_coord_send_aps(&coord, node[i].short_addr, 1, 1, 6, 0x0104, ZCL, 3, false);
            if (st == ZB_SEND_OK) { accepted++; accepted_to[i] = true; }
            else { CHECK(st == ZB_SEND_MAC_REFUSED); refused++; }
            /* zigbee-herdsman's concurrency with a Z-Stack 3.x adapter (16), plus the
             * inbound traffic of this loop: never refused. */
            if (sent_n < 16) CHECK(st == ZB_SEND_OK);
            if (++sent_n % 10 == 0) (void)report((i * 7) % NODES == CENTER ? 0 : (i * 7) % NODES);  /* inbound */
        }
        CHECK(refused > 0 && accepted >= MAC_DIRECT_QUEUE);
        pump();
        CHECK(confirms_total - conf == accepted);
        for (int i = 0; i < NODES; i++) if (accepted_to[i]) CHECK(delivered[i] == before[i] + 1);
        CHECK(coord.mac.active == -1 && coord.mac.direct_count == 0 && coord.mac.urgent_count == 0);
        for (unsigned k = 0; k < MAC_POOL_SLOTS; k++) CHECK(coord.mac.slots[k].kind == SLOT_FREE);
        for (unsigned k = 0; k < ZB_TX_TRACK; k++) CHECK(!coord.tx_track[k].used);
        unsigned retried = 0;
        for (int i = 0; i < NODES; i++) {
            if (i == CENTER || !node[i].alive || reach[i] == -2 || accepted_to[i]) continue;
            unsigned b = delivered[i];
            CHECK(zb_coord_send_aps(&coord, node[i].short_addr, 1, 1, 6, 0x0104, ZCL, 3, false) == ZB_SEND_OK);
            pump();
            CHECK(delivered[i] == b + 1);
            retried++;
        }
        CHECK(retried == refused);
        printf("mesh_scale: queue saturation: %u accepted, %u refused at once (MAC queue %u), all confirmed, "
               "refused ones delivered on retry, no slot leaked\n", accepted, refused, MAC_DIRECT_QUEUE);
    }

    /* ---- 9. broadcast storm: 24 routers broadcast at once, each copy
     *      heard from the coordinator's 4 neighbours, interleaved. Each
     *      broadcast goes up exactly once; then 80 at once: rebroadcast
     *      queue overflow is counted, nothing breaks. ---------------------- */
    {
        int nb[4], nn = 0;
        for (int i = 0; i < NODES && nn < 4; i++) if (i != CENTER && link_up(i, CENTER)) nb[nn++] = i;
        CHECK(nn >= 2);
        uint32_t up0 = coord.stats.aps_data_up, dup0 = coord.stats.broadcast_duplicates;
        uint8_t seqs[NODES];
        int storm[24], ns = 0;
        for (int i = 0; i < NODES && ns < 24; i++) if (i != CENTER && node[i].alive) storm[ns++] = i;
        for (int k = 0; k < ns; k++) seqs[storm[k]] = node[storm[k]].nwk_seq++;
        for (int copy = 0; copy < nn; copy++) {
            for (int k = 0; k < ns; k++) {
                int i = storm[k];
                const uint8_t aps[8 + 3] = {0x08, 0xFF, 0x06, 0x00, 0x04, 0x01, 0x01, (uint8_t)(0x90 + k), 0x01, 0x77, 0x02};
                nwk_header_t h = {0};
                h.type = NWK_TYPE_DATA; h.src = node[i].short_addr; h.dst = 0xFFFF; h.radius = 10;
                h.seq = seqs[i];
                deliver_to_coord(nb[copy], &h, aps, sizeof aps, true);
            }
        }
        unsigned up = coord.stats.aps_data_up - up0, dups = coord.stats.broadcast_duplicates - dup0;
        CHECK(up == (unsigned)ns);
        CHECK(dups == (unsigned)(ns * (nn - 1)));
        pump();
        uint32_t full0 = coord.stats.bcast_queue_full, up80 = coord.stats.aps_data_up;
        unsigned alive80 = 0;
        for (int i = 0; i < NODES; i++) {
            if (i == CENTER || !node[i].alive) continue;
            const uint8_t aps[8 + 3] = {0x08, 0xFF, 0x06, 0x00, 0x04, 0x01, 0x01, 0x33, 0x01, 0x78, 0x02};
            nwk_header_t h = {0};
            h.type = NWK_TYPE_DATA; h.src = node[i].short_addr; h.dst = 0xFFFF; h.radius = 10; h.seq = node[i].nwk_seq++;
            deliver_to_coord(nb[i % nn], &h, aps, sizeof aps, true);
            alive80++;
        }
        CHECK(coord.stats.bcast_queue_full > full0);
        CHECK(coord.stats.aps_data_up - up80 == alive80);   /* local delivery never depends on relay room */
        advance(10000);   /* pending rebroadcasts sent, BTT entries expire */
        CHECK(coord.mac.active == -1 && coord.mac.direct_count == 0);
        unsigned pending = 0;
        for (unsigned k = 0; k < ZB_BCAST_PENDING; k++) pending += coord.bcast[k].used;
        CHECK(pending == 0);
        live = live_connected();
        ok = send_all(2, &first);
        CHECK(ok == live);
        printf("mesh_scale: broadcast storm: %d broadcasts x %d copies -> %u delivered up, %u duplicates dropped; "
               "80 at once: %u rebroadcasts refused (queue %u), unicast traffic intact\n", ns, nn, up, dups,
               coord.stats.bcast_queue_full - full0, ZB_BCAST_PENDING);
    }

    /* ---- 10. router restarts: 10 routers (2 coordinator neighbours, 4
     *      corners) power off for 30 s and come back with their tables lost
     *      (frame counters kept, as in NV). Until a many-to-one request
     *      their subtree cannot report; the coordinator recovers alone. --- */
    {
        static const int restarted[10] = {CENTER + 1, CENTER - GRID, 0, GRID - 1, NODES - GRID, NODES - 1,
                                          2 * GRID + 2, 6 * GRID + 6, 3 * GRID + 1, 5 * GRID + 7};
        uint32_t auth0 = coord.stats.nwk_auth_failures;
        for (int k = 0; k < 10; k++) node[restarted[k]].alive = false;
        unsigned off_ok = 0;
        for (int k = 0; k < 10; k++) {   /* the host keeps sending to them */
            unsigned b = delivered[restarted[k]];
            (void)zb_coord_send_aps(&coord, node[restarted[k]].short_addr, 1, 1, 6, 0x0104, ZCL, 3, false);
            pump();
            off_ok += delivered[restarted[k]] != b;
        }
        CHECK(off_ok == 0);
        advance(30000);
        for (int k = 0; k < 10; k++) { node[restarted[k]].alive = true; node[restarted[k]].parent = -1; }
        /* A restarted relay lost its many-to-one route: its subtree is cut. */
        CHECK(!report(restarted[2]));
        uint32_t t0 = now, mt0 = coord.stats.mtorr_tx, nr0 = coord.stats.neighbor_restarts;
        /* The restarted coordinator neighbours send their first Link Status:
         * complete (first + last frame) and without us. */
        for (int k = 0; k < 2; k++) {
            const uint8_t ls[2] = {0x08, 0x60};
            nwk_header_t h = {0};
            h.type = NWK_TYPE_COMMAND; h.src = node[restarted[k]].short_addr; h.dst = 0xFFFC; h.radius = 1;
            h.seq = node[restarted[k]].nwk_seq++;
            deliver_to_coord(restarted[k], &h, ls, sizeof ls, true);
        }
        CHECK(coord.stats.neighbor_restarts == nr0 + 2);
        unsigned recovered_at = 0;
        for (unsigned step = 0; step < 400 && !recovered_at; step++) {
            advance(1000);
            if (coord.stats.mtorr_tx > mt0) recovered_at = now - t0;
        }
        CHECK(recovered_at > 0 && recovered_at <= ZB_EARLY_MTORR_MS + 1000u);   /* not the 2-minute periodic one */
        for (int i = 0; i < NODES; i++) if (i != CENTER && node[i].alive) CHECK(report(i));
        live = live_connected();
        CHECK(live == NODES - 1 - 8);   /* the 8 removed in phase 4 are still off */
        ok = send_all(3, &first);
        CHECK(ok == live);
        CHECK(coord.stats.nwk_auth_failures == auth0);   /* counters kept: frames accepted */
        printf("mesh_scale: 10 routers restarted: restart seen in the neighbours' Link Status, tree rebuilt "
               "%u ms after their return, "
               "%u/%u reached again\n", recovered_at, ok, live);
    }
    PASS("mesh_scale");
    return 0;
}
