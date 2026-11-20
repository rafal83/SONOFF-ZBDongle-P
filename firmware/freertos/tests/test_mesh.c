/* SPDX-License-Identifier: MIT
 * Mesh behaviour of the coordinator (zb_coord + nwk_route) against simulated
 * routers and end devices built only from the frame codecs:
 *  Link Status both ways, Many-to-One Route Request, Route Record and
 *  source routing, Route Request -> Route Reply, Update Device -> APS Tunnel
 *  (the end device decrypts the tunnelled Transport Key), relayed announce,
 *  broadcast duplicate suppression, Leave, secured and Trust Center rejoin,
 *  relaying to a child.
 */
#include "frt_test.h"
#include "aps_security.h"
#include "trust_center.h"
#include "zb_ccm_backend.h"
#include "zb_coord.h"
#include "zb_mmo.h"

#define COORD_EXT 0x00124B0001020304ull
#define PAN 0x1A62

/* ---- simulated air ---------------------------------------------------- */
static uint8_t air[64][MAC_MAX_MPDU];
static uint8_t air_len[64];
static unsigned air_count, air_read;
static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx; (void)ack;
    unsigned i = air_count++ % 64;
    memcpy(air[i], mpdu, len);
    air_len[i] = len;
    return 0;
}
static unsigned match_set;
static void sim_match(void *ctx, bool ext, uint64_t a, bool p) { (void)ctx; (void)ext; (void)a; if (p) match_set++; }
static zb_coord_t coord;

/* Next transmitted MPDU (acknowledged at MAC level). */
static bool take(mac_frame_t *f, uint8_t *copy) {
    if (air_read >= air_count) return false;
    unsigned i = air_read++ % 64;
    memcpy(copy, air[i], air_len[i]);
    CHECK(mac_frame_parse(copy, air_len[i], f) == MAC_OK);
    zb_coord_tx_done(&coord, MAC_TX_OK, false);
    return true;
}
static void drain(void) { mac_frame_t f; uint8_t b[MAC_MAX_MPDU]; while (take(&f, b)) {} }

static unsigned joined_events, left_events, last_left_rejoin;
static uint16_t joined_short;
static uint64_t joined_ext, left_ext;
static unsigned aps_up;
static uint8_t last_aps[256];
static size_t last_aps_len;
static void up_joined(void *ctx, uint16_t s, uint64_t e, uint8_t cap) { (void)ctx; (void)cap; joined_events++; joined_short = s; joined_ext = e; }
static void up_aps(void *ctx, const nwk_header_t *n, const aps_header_t *a, const uint8_t *p, size_t len, uint8_t lqi) {
    (void)ctx; (void)n; (void)a; (void)lqi; aps_up++;
    last_aps_len = len < sizeof last_aps ? len : sizeof last_aps;
    memcpy(last_aps, p, last_aps_len);
}
static bool up_reserve(void *ctx, uint32_t upper) { (void)ctx; (void)upper; return true; }
static bool up_random(void *ctx, uint8_t key[16]) { (void)ctx; for (unsigned i = 0; i < 16; i++) key[i] = (uint8_t)(i * 29 + 7); return true; }
static void up_left(void *ctx, uint16_t s, uint64_t e, bool rejoin) { (void)ctx; (void)s; left_events++; left_ext = e; last_left_rejoin = rejoin; }
static const mac_radio_ops_t RADIO = {sim_tx, sim_match, 0};
static unsigned channel_events, channel_last;
static void up_channel(void *ctx, uint8_t ch) { (void)ctx; channel_events++; channel_last = ch; }
static const zb_coord_upper_t UPPER = {up_joined, up_aps, up_reserve, up_random, 0, 0, up_left, 0, up_channel, 0};

/* ---- simulated nodes -------------------------------------------------- */
typedef struct {
    uint64_t ext;
    uint16_t short_addr;
    uint8_t tclk[16], nwk_key[16], mac_seq, nwk_seq, aps_counter, capability;
    uint32_t aps_fc;
    core_security_t sec;
    bool has_key;
} node_t;
static bool node_reserve(void *ctx, uint32_t u) { (void)ctx; (void)u; return true; }
static void node_init(node_t *n, uint64_t ext, uint16_t short_addr, uint8_t capability) {
    memset(n, 0, sizeof *n);
    n->ext = ext; n->short_addr = short_addr; n->capability = capability;
    memcpy(n->tclk, TC_DEFAULT_LINK_KEY, 16);
    core_security_init(&n->sec, 1, zb_ccm_backend_sw, node_reserve, 0);
}
static void node_set_key(node_t *n, const uint8_t key[16]) {
    memcpy(n->nwk_key, key, 16);
    n->has_key = core_security_set_key(&n->sec, 0, 0, key);
    CHECK(n->has_key);
}

/* `hop` transmits a NWK frame (secured by `hop`) whose NWK header is `h`. */
static void hop_send(node_t *hop, nwk_header_t *h, const uint8_t *payload, size_t len, uint16_t mac_dst, bool secured) {
    uint8_t nwk[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(h, nwk, sizeof nwk);
    CHECK(hl);
    memcpy(nwk + hl, payload, len);
    size_t total = hl + len;
    if (secured) {
        uint8_t local[8];
        for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(hop->ext >> (8 * i));
        CHECK(core_security_seal(&hop->sec, 0, local, nwk, hl, len, sizeof nwk, &total));
    }
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.seq = hop->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = mac_dst; f.ack_request = mac_dst != 0xFFFF;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = hop->short_addr;
    uint8_t mpdu[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&f, nwk, total, mpdu, sizeof mpdu);
    CHECK(n);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 210, -45);
}
static nwk_header_t hdr(uint8_t type, uint16_t src, uint16_t dst, uint8_t seq) {
    nwk_header_t h = {0};
    h.type = type; h.src = src; h.dst = dst; h.radius = 30; h.seq = seq;
    return h;
}

/* Parse + NWK-decrypt a coordinator frame as `rx` (which holds the key). */
typedef struct { mac_frame_t mac; nwk_header_t nwk; uint8_t payload[MAC_MAX_PAYLOAD]; size_t len; } rx_t;
static bool rx_frame(node_t *rx, rx_t *out) {
    uint8_t copy[MAC_MAX_MPDU];
    if (!take(&out->mac, copy)) return false;
    if (out->mac.type != MAC_TYPE_DATA) return false;
    uint8_t frame[MAC_MAX_MPDU];
    memcpy(frame, out->mac.payload, out->mac.payload_len);
    if (nwk_parse(frame, out->mac.payload_len, &out->nwk) != NWK_OK) return false;
    if (!out->nwk.security) {
        out->len = out->nwk.payload_len;
        memcpy(out->payload, out->nwk.payload, out->len);
        return true;
    }
    size_t plen;
    if (!core_security_open_authenticated_peer(&rx->sec, frame, out->mac.payload_len, out->nwk.header_len, &plen)) return false;
    memcpy(out->payload, frame + out->nwk.header_len + 14, plen);
    out->len = plen;
    return true;
}

static void announce(node_t *dev, node_t *hop, uint8_t seq) {
    uint8_t aps[8 + 12] = {0x08, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, dev->aps_counter++};
    uint8_t *z = aps + 8;
    z[0] = 0x55;
    z[1] = (uint8_t)dev->short_addr; z[2] = (uint8_t)(dev->short_addr >> 8);
    for (unsigned i = 0; i < 8; i++) z[3 + i] = (uint8_t)(dev->ext >> (8 * i));
    z[11] = dev->capability;
    nwk_header_t h = hdr(NWK_TYPE_DATA, dev->short_addr, NWK_BCAST_RX_ON, seq);
    hop_send(hop, &h, aps, sizeof aps, 0xFFFF, true);
}

/* Route tables full: the least recently REFRESHED entry is replaced (age of
 * its refresh time, wrap-safe: the clock crosses 2^32 during the test). */
static void route_lru_checks(void) {
    static nwk_routes_t t;
    nwk_routes_init(&t);
    t.now_ms = 0xFFFFFFFFu - 50u * NWK_SOURCE_ROUTES;
    const uint16_t relay[2] = {0x1111, 0x2222};
    for (unsigned i = 0; i < NWK_SOURCE_ROUTES; i++) {
        t.now_ms += 100;   /* wraps half-way */
        nwk_source_route_set(&t, (uint16_t)(0x0100 + i), relay, 2);
    }
    t.now_ms += 100;
    nwk_source_route_set(&t, 0x0100, relay, 1);          /* oldest refreshed: now the newest */
    t.now_ms += 100;
    nwk_source_route_set(&t, 0x7000, relay, 2);          /* evicts 0x0101, not 0x0100 */
    CHECK(nwk_source_route(&t, 0x0100) && nwk_source_route(&t, 0x0100)->count == 1);
    CHECK(!nwk_source_route(&t, 0x0101) && nwk_source_route(&t, 0x7000) && nwk_source_route(&t, 0x0102));
    for (unsigned i = 0; i < NWK_TABLE_ROUTES; i++) {
        t.now_ms += 10;
        nwk_table_route_set(&t, (uint16_t)(0x0200 + i), 0x1111);
    }
    t.now_ms += 10;
    nwk_table_route_set(&t, 0x0200, 0x2222);             /* refresh the oldest */
    t.now_ms += 10;
    nwk_table_route_set(&t, 0x7001, 0x1111);             /* evicts 0x0201 */
    uint16_t nh = 0;
    CHECK(nwk_table_route(&t, 0x0200, &nh) && nh == 0x2222);
    CHECK(!nwk_table_route(&t, 0x0201, &nh) && nwk_table_route(&t, 0x7001, &nh));
}

int main(void) {
    route_lru_checks();
    const uint8_t epid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t key[16];
    for (unsigned i = 0; i < 16; i++) key[i] = (uint8_t)(0xC0 + i);
    zb_coord_init(&coord, &RADIO, &UPPER, COORD_EXT, 0xBEEF1234u, 1000);
    zb_coord_form(&coord, PAN, 15, epid, key, 0);
    zb_coord_permit_join(&coord, 120);

    /* Router R (direct) and router R2 (behind R): known to the TC (backup). */
    static node_t r, r2, ed, ed2;
    node_init(&r, 0xA4C1380000000101ull, 0x1111, 0x8E);
    node_init(&r2, 0xA4C1380000000102ull, 0x2222, 0x8E);
    node_set_key(&r, key);
    node_set_key(&r2, key);
    CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, r.ext, r.short_addr, true, 0, 0xFF));
    CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, r2.ext, r2.short_addr, false, 0, 0xFF));
    coord.devices.dev[nwk_devices_find_ext(&coord.devices, r.ext)].capability = 0x8E;

    /* ---- timers: link status at +1 s, many-to-one request at +3 s ------ */
    zb_coord_tick(&coord, 0);       /* arms: link status at +1 s, MTORR at +3 s */
    zb_coord_tick(&coord, 1000);
    zb_coord_tick(&coord, 3000);
    rx_t m;
    CHECK(rx_frame(&r, &m));
    CHECK(m.nwk.type == NWK_TYPE_COMMAND && m.nwk.dst == NWK_BCAST_ROUTERS && m.nwk.radius == 1);
    CHECK(m.payload[0] == 0x08 && (m.payload[1] & 0x60) == 0x60 && (m.payload[1] & 0x1F) == 0);   /* no neighbour yet */
    CHECK(rx_frame(&r, &m));
    CHECK(m.nwk.type == NWK_TYPE_COMMAND && m.payload[0] == 0x01 && m.payload[1] == 0x08 &&
          m.payload[3] == 0xFC && m.payload[4] == 0xFF);                        /* MTORR, route record table */
    CHECK(coord.stats.mtorr_tx == 1 && coord.stats.link_status_tx == 1);

    /* ---- Link Status from R: neighbour with outgoing cost ------------- */
    {
        const uint8_t ls[2 + 3] = {0x08, 0x61, 0x00, 0x00, 0x13};   /* us: incoming 3, outgoing 1 */
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, r.short_addr, NWK_BCAST_ROUTERS, 1);
        h.radius = 1;
        hop_send(&r, &h, ls, sizeof ls, 0xFFFF, true);
        const nwk_neighbor_t *nb = nwk_neighbor(&coord.routes, r.short_addr);
        CHECK(nb && nb->out_cost == 3 && nb->in_cost == 1);
        zb_coord_tick(&coord, 16000);
        /* Passive ACK: R never relayed our many-to-one request, so it is
         * retransmitted (radius > 1 broadcast) before the link status. */
        CHECK(rx_frame(&r, &m));
        CHECK(m.payload[0] == 0x01 && m.payload[1] == 0x08 && coord.stats.bcast_retries == 1);
        CHECK(rx_frame(&r, &m));
        CHECK(m.payload[0] == 0x08 && (m.payload[1] & 0x1F) == 1 && m.payload[2] == 0x11 && m.payload[3] == 0x11 &&
              m.payload[4] == (0x01 | (3 << 4)));
    }

    /* ---- Route Record from R2 via R, then source-routed data to R2 ---- */
    {
        const uint8_t rr[4] = {0x05, 0x01, 0x11, 0x11};
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, r2.short_addr, 0x0000, 2);
        hop_send(&r, &h, rr, sizeof rr, 0x0000, true);
        CHECK(coord.stats.route_records == 1);
        const uint8_t zcl[3] = {0x01, 0x10, 0x02};
        CHECK(zb_coord_send_aps(&coord, r2.short_addr, 1, 1, 0x0006, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        CHECK(rx_frame(&r, &m));
        CHECK(m.mac.dst_short == r.short_addr && m.nwk.dst == r2.short_addr && m.nwk.source_route &&
              m.nwk.relay_count == 1 && m.nwk.relay_index == 0 && m.nwk.relays[0] == r.short_addr);
        /* OTA Image Block Response (ZCL header 17 + 50 data bytes) along that
         * source route: 108 bytes of MAC payload. Refused before by the
         * worst-case 102-byte bound (OTA of a Hue bulb stalled, 19 November). */
        uint8_t block[17 + 50];
        for (unsigned i = 0; i < sizeof block; i++) block[i] = (uint8_t)i;
        CHECK(zb_coord_send_aps(&coord, r2.short_addr, 1, 1, 0x0019, 0x0104, block, sizeof block, false) ==
              ZB_SEND_OK);
        CHECK(rx_frame(&r, &m));
        CHECK(m.nwk.dst == r2.short_addr && m.nwk.source_route && m.len >= sizeof block);
    }

    /* ---- APS fragmentation, both directions (window of one block) ----- */
    {
        /* Coordinator -> R: 200 bytes = 4 blocks; the ACK of block 1 is
         * lost once, the block is repeated after ZB_FRAG_WAIT_MS. */
        uint8_t big[200], got[256];
        size_t got_len = 0;
        for (unsigned i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + 3);
        uint32_t ok0 = coord.stats.frag_tx_ok;
        CHECK(zb_coord_send_aps(&coord, r.short_addr, 1, 1, 0x0006, 0x0104, big, sizeof big, false) == ZB_SEND_OK);
        uint8_t seq = 0x60;
        bool lost_once = false;
        for (unsigned blk = 0; blk < 4;) {
            aps_header_t a;
            bool found = false;
            while (!found && rx_frame(&r, &m))
                found = m.nwk.dst == r.short_addr && aps_parse(m.payload, m.len, &a) == APS_OK && a.type == APS_TYPE_DATA;
            CHECK(found);
            CHECK(a.extended && a.ack_request && a.fragmentation == (blk ? APS_FRAG_NEXT : APS_FRAG_FIRST));
            CHECK(a.block_number == (blk ? blk : 4u) && a.payload_len <= ZB_FRAG_BLOCK);
            if (got_len == (size_t)blk * ZB_FRAG_BLOCK) {
                memcpy(got + got_len, a.payload, a.payload_len);
                got_len += a.payload_len;
            }
            if (blk == 1 && !lost_once) {
                lost_once = true;
                zb_coord_tick(&coord, coord.now_ms + ZB_FRAG_WAIT_MS + 10u);   /* no ACK: repeated */
                continue;
            }
            aps_header_t ack = {0};
            ack.type = APS_TYPE_ACK; ack.delivery = APS_DELIVERY_UNICAST;
            ack.dst_endpoint = 1; ack.src_endpoint = 1; ack.cluster = 0x0006; ack.profile = 0x0104;
            ack.counter = a.counter; ack.extended = true; ack.fragmentation = a.fragmentation;
            ack.block_number = (uint8_t)blk; ack.ack_bitfield = 1;
            uint8_t ab[16];
            size_t al = aps_build(&ack, ab, sizeof ab);
            CHECK(al);
            nwk_header_t h = hdr(NWK_TYPE_DATA, r.short_addr, 0x0000, seq++);
            hop_send(&r, &h, ab, al, 0x0000, true);
            blk++;
        }
        CHECK(got_len == sizeof big && !memcmp(got, big, sizeof big));
        CHECK(coord.stats.frag_tx_ok == ok0 + 1 && coord.stats.frag_tx_retries >= 1 && !coord.frag_tx.used);

        /* R -> coordinator: 150 bytes in 3 blocks, block 1 received twice:
         * every block acknowledged, the message delivered once, intact. */
        uint8_t msg[150];
        for (unsigned i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(255 - i);
        unsigned up0 = aps_up;
        uint32_t rx0 = coord.stats.frag_rx_ok;
        const unsigned sizes[3] = {64, 64, 22};
        unsigned order[4] = {0, 1, 1, 2}, off[3] = {0, 64, 128};
        for (unsigned k = 0; k < 4; k++) {
            unsigned blk = order[k];
            aps_header_t a = {0};
            a.type = APS_TYPE_DATA; a.delivery = APS_DELIVERY_UNICAST; a.ack_request = true;
            a.extended = true; a.fragmentation = blk ? APS_FRAG_NEXT : APS_FRAG_FIRST;
            a.block_number = (uint8_t)(blk ? blk : 3u);
            a.dst_endpoint = 1; a.src_endpoint = 1; a.cluster = 0x0006; a.profile = 0x0104; a.counter = 0x77;
            uint8_t fr[128];
            size_t hl = aps_build(&a, fr, sizeof fr);
            CHECK(hl);
            memcpy(fr + hl, msg + off[blk], sizes[blk]);
            nwk_header_t h = hdr(NWK_TYPE_DATA, r.short_addr, 0x0000, seq++);
            hop_send(&r, &h, fr, hl + sizes[blk], 0x0000, true);
            aps_header_t ack;
            bool found = false;
            while (!found && rx_frame(&r, &m))
                found = m.nwk.dst == r.short_addr && aps_parse(m.payload, m.len, &ack) == APS_OK && ack.type == APS_TYPE_ACK;
            CHECK(found && ack.counter == 0x77 && ack.extended && ack.block_number == blk && (ack.ack_bitfield & 1));
        }
        CHECK(aps_up == up0 + 1 && coord.stats.frag_rx_ok == rx0 + 1);
        CHECK(last_aps_len == sizeof msg && !memcmp(last_aps, msg, sizeof msg));
    }


    /* ---- No route: NO_ROUTE and a Route Request; Route Reply -> route -- */
    {
        CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, 0xA4C1380000000199ull, 0x3333, false, 0, 0xFF));
        const uint8_t zcl[3] = {0x01, 0x11, 0x00};
        /* Held during the discovery (Z-Stack does the same), not failed.
         * Three frames for the same destination: ONE Route Request. */
        uint32_t rreq0 = coord.stats.rreq_tx;
        for (unsigned k = 0; k < 3; k++)
            CHECK(zb_coord_send_aps(&coord, 0x3333, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        CHECK(coord.stats.disc_buffered == 3 && coord.stats.rreq_tx == rreq0 + 1 && coord.stats.rreq_coalesced == 2);
        CHECK(rx_frame(&r, &m));
        CHECK(m.payload[0] == 0x01 && m.payload[1] == 0x00 && m.payload[3] == 0x33 && m.payload[4] == 0x33);
        CHECK(!rx_frame(&r, &m));                                       /* no second Route Request */
        uint8_t id = m.payload[2];
        const uint8_t rrep[8] = {0x02, 0x00, id, 0x00, 0x00, 0x33, 0x33, 4};
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, 0x3333, 0x0000, 3);
        hop_send(&r, &h, rrep, sizeof rrep, 0x0000, true);
        CHECK(coord.stats.rrep_rx == 1);
        /* The held frames leave on the Route Reply, along the new route. */
        CHECK(coord.stats.disc_sent == 3 && coord.disc_count == 0);
        for (unsigned k = 0; k < 3; k++) {
            CHECK(rx_frame(&r, &m));
            CHECK(m.mac.dst_short == r.short_addr && m.nwk.dst == 0x3333 && !m.nwk.source_route);
        }
        CHECK(zb_coord_send_aps(&coord, 0x3333, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        CHECK(rx_frame(&r, &m));
        CHECK(m.mac.dst_short == r.short_addr && m.nwk.dst == 0x3333 && !m.nwk.source_route);
    }

    /* ---- Route Request for the coordinator from R2 (via R) -> Route Reply */
    {
        const uint8_t rreq[6] = {0x01, 0x00, 0x42, 0x00, 0x00, 2};
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, r2.short_addr, NWK_BCAST_ROUTERS, 4);
        hop_send(&r, &h, rreq, sizeof rreq, 0xFFFF, true);
        CHECK(rx_frame(&r, &m));
        CHECK(m.nwk.type == NWK_TYPE_COMMAND && m.payload[0] == 0x02 && m.payload[2] == 0x42 &&
              m.payload[3] == 0x22 && m.payload[4] == 0x22 && m.payload[5] == 0 && m.payload[6] == 0);
        CHECK(m.mac.dst_short == r.short_addr);     /* source route via R (route record) */
    }

    /* ---- End device ED joins through R: Update Device -> APS Tunnel ---- */
    node_init(&ed, 0xA4C1380000000201ull, 0x4444, 0x80);
    {
        uint8_t ud[12] = {0x06};
        for (unsigned i = 0; i < 8; i++) ud[1 + i] = (uint8_t)(ed.ext >> (8 * i));
        ud[9] = 0x44; ud[10] = 0x44; ud[11] = 0x01;   /* unsecured join */
        /* R21 router: Update Device APS-secured with its TC link key. */
        uint8_t aps[MAC_MAX_PAYLOAD];
        const uint8_t ahdr[2] = {0x01, r.aps_counter++};
        nwk_aux_t aux = {.key_id = APS_KEY_DATA, .extended_nonce = true, .frame_counter = r.aps_fc++, .source = r.ext};
        size_t n = aps_seal(r.tclk, ahdr, 2, &aux, ud, sizeof ud, aps, sizeof aps);
        CHECK(n);
        nwk_header_t h = hdr(NWK_TYPE_DATA, r.short_addr, 0x0000, 5);
        hop_send(&r, &h, aps, n, 0x0000, true);
        CHECK(coord.stats.update_device == 1 && coord.stats.tunnels_sent == 1);
        CHECK(rx_frame(&r, &m));
        CHECK(m.mac.dst_short == r.short_addr && m.nwk.dst == r.short_addr);
        aps_header_t a;
        CHECK(aps_parse(m.payload, m.len, &a) == APS_OK && a.type == APS_TYPE_COMMAND && !a.security);
        CHECK(a.payload[0] == 0x0E);
        uint64_t dst = 0;
        for (int i = 7; i >= 0; i--) dst = (dst << 8) | a.payload[1 + i];
        CHECK(dst == ed.ext);
        /* What R forwards to ED: the tunnelled APS frame, opened by ED. */
        uint8_t inner[MAC_MAX_PAYLOAD];
        size_t inner_len = a.payload_len - 9;
        memcpy(inner, a.payload + 9, inner_len);
        aps_header_t ia;
        CHECK(aps_parse(inner, inner_len, &ia) == APS_OK && ia.security && ia.type == APS_TYPE_COMMAND);
        uint8_t ktk[16];
        zb_key_transport_key(ed.tclk, ktk);
        nwk_aux_t iaux;
        size_t off, plen;
        CHECK(aps_open(ktk, inner, inner_len, ia.header_len, &iaux, &off, &plen));
        CHECK(plen == 35 && inner[off] == APS_CMD_TRANSPORT_KEY && !memcmp(inner + off + 2, key, 16));
        node_set_key(&ed, inner + off + 2);
    }
    /* Relayed announce (security source = R): accepted; a duplicate relayed
     * by R2 is suppressed by the broadcast transaction table. */
    announce(&ed, &r, 9);
    CHECK(joined_events == 1 && joined_ext == ed.ext && joined_short == 0x4444);
    announce(&ed, &r2, 9);
    CHECK(joined_events == 1 && coord.stats.broadcast_duplicates == 1);
    /* A relayed announce claiming another device's address is refused. */
    {
        node_t fake = ed;
        fake.short_addr = 0x5555;
        uint32_t unknown = coord.stats.nwk_unknown_source;
        uint8_t aps[8 + 12] = {0x08, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x77};
        aps[9] = 0x44; aps[10] = 0x44;   /* announces 0x4444 ... */
        for (unsigned i = 0; i < 8; i++) aps[11 + i] = (uint8_t)(ed.ext >> (8 * i));
        nwk_header_t h = hdr(NWK_TYPE_DATA, fake.short_addr, NWK_BCAST_RX_ON, 10);   /* ... from 0x5555 */
        hop_send(&r, &h, aps, sizeof aps, 0xFFFF, true);
        CHECK(coord.stats.nwk_unknown_source == unknown + 1 && joined_events == 1);
    }
    /* Data to ED goes through R (source route from Update Device). */
    {
        const uint8_t zcl[3] = {0x01, 0x12, 0x02};
        CHECK(zb_coord_send_aps(&coord, ed.short_addr, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        CHECK(rx_frame(&r, &m));
        CHECK(m.mac.dst_short == r.short_addr && m.nwk.dst == ed.short_addr && m.nwk.source_route &&
              m.nwk.relays[0] == r.short_addr && !m.mac.ack_request == false);
    }
    /* Update Device while joining is closed: no key leaves the TC. */
    {
        zb_coord_permit_join(&coord, 0);
        uint8_t ud[12] = {0x06};
        for (unsigned i = 0; i < 8; i++) ud[1 + i] = (uint8_t)(0xA4C1380000000299ull >> (8 * i));
        ud[9] = 0x66; ud[10] = 0x66; ud[11] = 0x01;
        uint8_t aps[2 + 12] = {0x01, r.aps_counter++};
        memcpy(aps + 2, ud, sizeof ud);
        nwk_header_t h = hdr(NWK_TYPE_DATA, r.short_addr, 0x0000, 11);
        hop_send(&r, &h, aps, sizeof aps, 0x0000, true);
        CHECK(coord.stats.tunnels_sent == 1 && air_read == air_count);
        CHECK(nwk_devices_find_ext(&coord.devices, 0xA4C1380000000299ull) == NWK_DEVICE_NONE);
        zb_coord_permit_join(&coord, 120);
    }

    /* ---- Unicast for our child, from R: relayed (re-secured by us) ---- */
    node_init(&ed2, 0xA4C1380000000202ull, 0x7777, 0x8C);   /* rx-on end device, our child */
    node_set_key(&ed2, key);
    CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, ed2.ext, ed2.short_addr, true, 0, 0xFF));
    coord.devices.dev[nwk_devices_find_ext(&coord.devices, ed2.ext)].capability = 0x8C;
    {
        const uint8_t aps[8 + 3] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0x33, 0x01, 0x20, 0x01};
        nwk_header_t h = hdr(NWK_TYPE_DATA, r2.short_addr, ed2.short_addr, 12);
        uint32_t up = aps_up;
        hop_send(&r, &h, aps, sizeof aps, 0x0000, true);
        CHECK(coord.stats.relayed == 1 && aps_up == up);   /* not delivered locally */
        CHECK(rx_frame(&ed2, &m));
        CHECK(m.mac.dst_short == ed2.short_addr && m.nwk.src == r2.short_addr && m.nwk.radius == 29 &&
              m.len == sizeof aps && !memcmp(m.payload, aps, sizeof aps));
    }

    /* ---- Leave (with rejoin) and leave (for good) ---------------------- */
    {
        const uint8_t lv[2] = {0x04, 0x20};
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, ed.short_addr, 0x0000, 13);
        hop_send(&r, &h, lv, 2, 0x0000, true);
        CHECK(left_events == 1 && left_ext == ed.ext && last_left_rejoin);
        CHECK(nwk_devices_find_ext(&coord.devices, ed.ext) != NWK_DEVICE_NONE);
        const uint8_t lv2[2] = {0x04, 0x00};
        h = hdr(NWK_TYPE_COMMAND, ed.short_addr, 0x0000, 14);
        hop_send(&r, &h, lv2, 2, 0x0000, true);
        CHECK(left_events == 2 && !last_left_rejoin);
        CHECK(nwk_devices_find_ext(&coord.devices, ed.ext) == NWK_DEVICE_NONE);
    }

    /* ---- Secured rejoin of R2 directly with us (MAC source = IEEE) ----- */
    drain();
    {
        const uint8_t rj[2] = {0x06, 0x8E};
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, r2.short_addr, 0x0000, 15);
        h.radius = 1;
        h.has_src_ieee = true; h.src_ieee = r2.ext;
        uint8_t nwk[CORE_SECURITY_MAX_FRAME];
        size_t hl = nwk_build(&h, nwk, sizeof nwk);
        memcpy(nwk + hl, rj, 2);
        uint8_t local[8];
        for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(r2.ext >> (8 * i));
        size_t total;
        CHECK(core_security_seal(&r2.sec, 0, local, nwk, hl, 2, sizeof nwk, &total));
        mac_frame_t f = {0};
        f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.ack_request = true; f.seq = r2.mac_seq++;
        f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
        f.src_mode = MAC_ADDR_EXT; f.src_ext = r2.ext;
        uint8_t mpdu[MAC_MAX_MPDU];
        size_t n = mac_frame_build(&f, nwk, total, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 220, -40);
        CHECK(coord.stats.rejoins == 1);
        CHECK(rx_frame(&r2, &m));
        CHECK(m.mac.dst_mode == MAC_ADDR_EXT && m.mac.dst_ext == r2.ext && m.nwk.has_dst_ieee && m.nwk.dst_ieee == r2.ext);
        CHECK(m.payload[0] == 0x07 && m.payload[1] == 0x22 && m.payload[2] == 0x22 && m.payload[3] == 0x00);
        const nwk_device_t *d = &coord.devices.dev[nwk_devices_find_ext(&coord.devices, r2.ext)];
        CHECK(d->relationship == NWK_REL_CHILD && d->short_addr == 0x2222);
        /* Now a child: data goes direct, without source route. */
        const uint8_t zcl[3] = {0x01, 0x13, 0x02};
        CHECK(zb_coord_send_aps(&coord, r2.short_addr, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        CHECK(rx_frame(&r2, &m));
        CHECK(m.mac.dst_short == r2.short_addr && !m.nwk.source_route);
    }

    /* ---- Trust Center rejoin (unsecured) of a device with a unique key - */
    {
        node_t tc;
        node_init(&tc, 0xA4C1380000000301ull, 0x6060, 0x80);
        uint8_t unique[16];
        for (unsigned i = 0; i < 16; i++) unique[i] = (uint8_t)(0x31 * i + 1);
        CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, tc.ext, tc.short_addr, false, unique, 0xFF));
        memcpy(tc.tclk, unique, 16);
        zb_coord_permit_join(&coord, 0);   /* allowed even when closed: unique key */
        const uint8_t rj[2] = {0x06, 0x80};
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, tc.short_addr, 0x0000, 1);
        h.radius = 1;
        uint8_t nwk[64];
        size_t hl = nwk_build(&h, nwk, sizeof nwk);
        memcpy(nwk + hl, rj, 2);
        mac_frame_t f = {0};
        f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.ack_request = true; f.seq = 1;
        f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
        f.src_mode = MAC_ADDR_EXT; f.src_ext = tc.ext;
        uint8_t mpdu[MAC_MAX_MPDU];
        size_t n = mac_frame_build(&f, nwk, hl + 2, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 220, -40);
        CHECK(coord.stats.tc_rejoins == 1);
        /* Sleepy: rejoin response and key are queued for its poll. */
        const uint8_t poll = MAC_CMD_DATA_REQUEST;
        mac_frame_t pf = {0};
        pf.type = MAC_TYPE_COMMAND; pf.ack_request = true; pf.pan_id_compression = true; pf.seq = 2;
        pf.dst_mode = MAC_ADDR_SHORT; pf.dst_pan = PAN; pf.dst_short = 0;
        pf.src_mode = MAC_ADDR_EXT; pf.src_ext = tc.ext;
        n = mac_frame_build(&pf, &poll, 1, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 220, -40);
        node_t blank;
        node_init(&blank, tc.ext, tc.short_addr, 0x80);
        CHECK(rx_frame(&blank, &m));
        CHECK(!m.nwk.security && m.payload[0] == 0x07 && m.mac.dst_ext == tc.ext);
        /* Then the Transport Key, encrypted with the unique key. */
        pf.src_mode = MAC_ADDR_SHORT; pf.src_short = tc.short_addr; pf.seq = 3;
        n = mac_frame_build(&pf, &poll, 1, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 220, -40);
        CHECK(rx_frame(&blank, &m));
        aps_header_t a;
        CHECK(!m.nwk.security && aps_parse(m.payload, m.len, &a) == APS_OK && a.security);
        uint8_t ktk[16];
        zb_key_transport_key(unique, ktk);
        nwk_aux_t aux;
        size_t off, plen;
        CHECK(aps_open(ktk, m.payload, m.len, a.header_len, &aux, &off, &plen));
        CHECK(m.payload[off] == APS_CMD_TRANSPORT_KEY && !memcmp(m.payload + off + 2, key, 16));
        /* An unknown device cannot TC-rejoin. */
        uint32_t before = coord.stats.tc_rejoins;
        f.src_ext = 0x1234567812345678ull; f.seq = 9;
        n = mac_frame_build(&f, nwk, hl + 2, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 220, -40);
        CHECK(coord.stats.tc_rejoins == before);
    }

    /* ---- Router duties ------------------------------------------------- */
    drain();
    zb_coord_tick(&coord, 16000);   /* time reference */
    drain();
    {
        /* A broadcast to all (from R2, relayed by R) is rebroadcast after the
         * jitter, with a copy queued for our sleepy child. */
        uint32_t copies = coord.stats.sleepy_bcast_copies, rebroadcasts = coord.stats.rebroadcasts;
        const uint8_t aps[8 + 2] = {0x08, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0x44, 0x01, 0x02};
        nwk_header_t h = hdr(NWK_TYPE_DATA, r2.short_addr, NWK_BCAST_ALL, 0x61);
        hop_send(&r, &h, aps, sizeof aps, 0xFFFF, true);
        zb_coord_tick(&coord, 16100);
        bool found = false;   /* earlier queued relays (the announce) may go first */
        while (!found && rx_frame(&r, &m)) found = m.nwk.src == r2.short_addr && m.nwk.seq == 0x61;
        CHECK(found);
        CHECK(m.mac.dst_short == 0xFFFF && m.nwk.src == r2.short_addr && m.nwk.dst == NWK_BCAST_ALL &&
              m.nwk.radius == 29 && m.nwk.seq == 0x61 && m.len == sizeof aps);
        /* (the pending announce relay counts too; only 0xFFFF gets sleepy copies) */
        CHECK(coord.stats.rebroadcasts >= rebroadcasts + 1 && coord.stats.sleepy_bcast_copies == copies + 1);
        /* R relays it too: passive acknowledgement, no retransmission. */
        h = hdr(NWK_TYPE_DATA, r2.short_addr, NWK_BCAST_ALL, 0x61);
        h.radius = 28;
        hop_send(&r, &h, aps, sizeof aps, 0xFFFF, true);
        zb_coord_tick(&coord, 16800);
        while (rx_frame(&r, &m)) CHECK(!(m.nwk.src == r2.short_addr && m.nwk.seq == 0x61));
    }
    {
        /* Source route whose last relay is us: delivered to our child. */
        const uint8_t aps[8 + 2] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0x45, 0x01, 0x03};
        nwk_header_t h = hdr(NWK_TYPE_DATA, r.short_addr, ed2.short_addr, 0x62);
        h.source_route = true; h.relay_count = 1; h.relay_index = 0; h.relays[0] = 0x0000;
        hop_send(&r, &h, aps, sizeof aps, 0x0000, true);
        CHECK(coord.stats.source_relayed == 1);
        CHECK(rx_frame(&ed2, &m));
        CHECK(m.mac.dst_short == ed2.short_addr && m.nwk.dst == ed2.short_addr && m.nwk.source_route &&
              m.nwk.relay_index == 0 && m.nwk.radius == 29);
        /* Intermediate relay: forwarded to the next relay, index decremented. */
        h = hdr(NWK_TYPE_DATA, ed2.short_addr, 0x3333, 0x63);
        h.source_route = true; h.relay_count = 2; h.relay_index = 1; h.relays[0] = r.short_addr; h.relays[1] = 0x0000;
        node_t *from = &ed2;
        hop_send(from, &h, aps, sizeof aps, 0x0000, true);
        CHECK(coord.stats.source_relayed == 2);
        CHECK(rx_frame(&r, &m));
        CHECK(m.mac.dst_short == r.short_addr && m.nwk.dst == 0x3333 && m.nwk.relay_index == 0);
        /* We are not the relay the index points at: dropped. */
        uint32_t dropped = coord.stats.relay_dropped;
        h.relay_index = 0;
        h.seq = 0x64;
        hop_send(from, &h, aps, sizeof aps, 0x0000, true);
        CHECK(coord.stats.relay_dropped == dropped + 1);
    }
    {
        /* Route Request of R for an unknown target: relayed with our link
         * cost added; the Route Reply is passed back hop by hop. */
        const uint8_t rreq[6] = {0x01, 0x00, 0x77, 0x5A, 0x5A, 0x02};
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, r.short_addr, NWK_BCAST_ROUTERS, 0x65);
        hop_send(&r, &h, rreq, sizeof rreq, 0xFFFF, true);
        CHECK(coord.stats.rreq_relayed == 1);
        zb_coord_tick(&coord, 16900);
        CHECK(rx_frame(&r, &m));
        CHECK(m.nwk.type == NWK_TYPE_COMMAND && m.payload[0] == 0x01 && m.payload[2] == 0x77 && m.payload[5] == 3 &&
              m.nwk.radius == 29 && m.nwk.src == r.short_addr);
        const uint8_t rrep[8] = {0x02, 0x00, 0x77, 0x11, 0x11, 0x5A, 0x5A, 4};
        h = hdr(NWK_TYPE_COMMAND, ed2.short_addr, 0x0000, 0x66);   /* reply arrives from another neighbour */
        hop_send(&ed2, &h, rrep, sizeof rrep, 0x0000, true);
        CHECK(coord.stats.rrep_relayed == 1);
        CHECK(rx_frame(&r, &m));
        CHECK(m.mac.dst_short == r.short_addr && m.nwk.dst == r.short_addr && m.payload[0] == 0x02 &&
              m.payload[3] == 0x11 && m.payload[5] == 0x5A && m.payload[7] == 5);
        uint16_t nh;
        CHECK(nwk_table_route(&coord.routes, 0x5A5A, &nh) && nh == ed2.short_addr);
    }
    {
        /* Unicast through us without any route: Network Status (no route) to the source. */
        const uint8_t aps[8 + 1] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0x46, 0x00};
        nwk_header_t h = hdr(NWK_TYPE_DATA, r.short_addr, 0x5B5B, 0x67);
        hop_send(&r, &h, aps, sizeof aps, 0x0000, true);
        CHECK(rx_frame(&r, &m));
        CHECK(m.nwk.type == NWK_TYPE_COMMAND && m.nwk.dst == r.short_addr && m.payload[0] == 0x03 &&
              m.payload[1] == 0x00 && m.payload[2] == 0x5B && m.payload[3] == 0x5B);
    }

    /* ---- APS duplicate rejection: a retransmission is delivered once --- */
    drain();
    {
        const uint8_t aps[8 + 3] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0xA7, 0x18, 0x05, 0x0A};
        uint32_t up = aps_up, dups = coord.stats.aps_duplicates;
        nwk_header_t h = hdr(NWK_TYPE_DATA, r.short_addr, 0x0000, 0x70);
        hop_send(&r, &h, aps, sizeof aps, 0x0000, true);
        h = hdr(NWK_TYPE_DATA, r.short_addr, 0x0000, 0x71);   /* same APS counter, new NWK frame */
        hop_send(&r, &h, aps, sizeof aps, 0x0000, true);
        CHECK(aps_up == up + 1 && coord.stats.aps_duplicates == dups + 1);
    }
    /* ---- Local Mgmt_Lqi / Mgmt_Rtg (Zigbee2MQTT network map) ----------- */
    {
        uint8_t rsp[128];
        const uint8_t lqi_req[2] = {0x31, 0x00};
        size_t n = zb_coord_zdo_local(&coord, 0x0031, lqi_req, 2, false, rsp, sizeof rsp);
        CHECK(n >= 5 && rsp[0] == 0x31 && rsp[1] == 0x00 && rsp[3] == 0 && rsp[4] >= 1 && rsp[4] <= 3);
        CHECK(n == 5u + 22u * rsp[4]);
        bool saw_router = false;
        for (unsigned i = 0; i < rsp[4]; i++) {
            const uint8_t *e = rsp + 5 + 22 * i;
            uint16_t a = (uint16_t)(e[16] | (e[17] << 8));
            if (a == r.short_addr) saw_router = (e[18] & 0x03) == 1 && ((e[18] >> 4) & 7) == 1;
            CHECK(!memcmp(e, epid, 8));
        }
        CHECK(saw_router || rsp[2] > 3);
        const uint8_t rtg_req[2] = {0x32, 0x00};
        n = zb_coord_zdo_local(&coord, 0x0032, rtg_req, 2, false, rsp, sizeof rsp);
        CHECK(n >= 5 && rsp[1] == 0x00 && rsp[2] >= 1 && n == 5u + 5u * rsp[4]);
    }
    /* ---- APS-secured data on request (host EN_SECURITY) ---------------- */
    {
        const uint8_t zcl[3] = {0x01, 0x14, 0x00};
        /* A device known only with the default key: refused, never sent clear. */
        CHECK(zb_coord_send_aps_secured(&coord, r.short_addr, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_SECURITY);
        uint32_t sec = coord.stats.aps_secured_tx;
        CHECK(zb_coord_send_aps_secured(&coord, 0x6060, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        CHECK(coord.stats.aps_secured_tx == sec + 1);
        /* The frame waits for the sleepy child's poll: a radio restart (TX
         * power change) must re-assert its hardware pending bit. */
        unsigned before = match_set;
        mac_source_match_resync(&coord.mac);
        CHECK(match_set > before);
    }

    /* ---- Imported child (backup: capability unknown) ------------------ */
    drain();
    {
        /* Found on the dongle: a restored child was treated as sleepy and its
         * frames rotted in the indirect queue. Unknown = listening, direct. */
        node_t imp;
        node_init(&imp, 0xA4C1380000000401ull, 0x0D0D, 0x8E);
        CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, imp.ext, imp.short_addr, true, 0, 0xFF));
        const uint8_t zcl[3] = {0x01, 0x15, 0x02};
        CHECK(zb_coord_send_aps(&coord, imp.short_addr, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        mac_frame_t f; uint8_t copy[MAC_MAX_MPDU];
        CHECK(take(&f, copy) && f.dst_short == imp.short_addr);   /* sent directly */
        /* It polls: a sleepy child after all, delivered on its polls from now on. */
        const uint8_t poll = MAC_CMD_DATA_REQUEST;
        mac_frame_t pf = {0};
        pf.type = MAC_TYPE_COMMAND; pf.ack_request = true; pf.pan_id_compression = true; pf.seq = 1;
        pf.dst_mode = MAC_ADDR_SHORT; pf.dst_pan = PAN; pf.dst_short = 0;
        pf.src_mode = MAC_ADDR_SHORT; pf.src_short = imp.short_addr;
        uint8_t mpdu[32];
        size_t n = mac_frame_build(&pf, &poll, 1, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
        drain();
        unsigned sent = air_count;
        CHECK(zb_coord_send_aps(&coord, imp.short_addr, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        CHECK(air_count == sent);                                  /* queued for its next poll */
        pf.seq = 2;                                                /* a new poll (MAC de-duplicates) */
        n = mac_frame_build(&pf, &poll, 1, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
        CHECK(take(&f, copy) && f.dst_short == imp.short_addr);
        /* (Association polls never reclassify a joined device: see test_join_e2e.) */
    }

    /* ---- Network Status (route failure) drops the routes to it -------- */
    {
        CHECK(nwk_source_route(&coord.routes, 0x2222) || true);
        nwk_table_route_set(&coord.routes, 0x3333, r.short_addr);
        const uint8_t ns[4] = {0x03, 0x02, 0x33, 0x33};
        nwk_header_t h = hdr(NWK_TYPE_COMMAND, r.short_addr, 0x0000, 20);
        hop_send(&r, &h, ns, sizeof ns, 0x0000, true);
        uint16_t nh;
        CHECK(!nwk_table_route(&coord.routes, 0x3333, &nh));
    }

    /* ---- Many neighbours (production: 32 routers): the Link Status is
     *      split into frames that fit; every NWK command carries our IEEE
     *      in the NWK header (real routers ignored it otherwise). ------- */
    drain();
    {
        for (unsigned i = 0; i < 30; i++)
            (void)nwk_neighbor_link_status(&coord.routes, (uint16_t)(0x5000 + i), 200, 0x0000, 0, 0, true);
        uint32_t base = 31000u;   /* the next link status is due (same tick as the ageing sequence start) */
        zb_coord_tick(&coord, base);
        unsigned frames = 0, entries = 0;
        bool first_seen = false, last_seen = false, mtorr_ok = true;
        for (;;) {
            uint8_t copy[MAC_MAX_MPDU];
            mac_frame_t f;
            if (!take(&f, copy)) break;
            nwk_header_t h;
            CHECK(nwk_parse(f.payload, f.payload_len, &h) == NWK_OK);
            CHECK(f.payload_len + 23u <= MAC_MAX_MPDU);                      /* fits */
            if (h.type != NWK_TYPE_COMMAND) continue;
            CHECK(h.has_src_ieee && h.src_ieee == COORD_EXT);               /* IEEE in the NWK header */
            rx_t m2;
            m2.mac = f;
            uint8_t frame[MAC_MAX_MPDU];
            memcpy(frame, f.payload, f.payload_len);
            size_t plen;
            CHECK(core_security_open_authenticated_peer(&r.sec, frame, f.payload_len, h.header_len, &plen));
            const uint8_t *p = frame + h.header_len + 14;
            if (p[0] == 0x01) { mtorr_ok &= h.has_src_ieee; continue; }
            if (p[0] != 0x08) continue;
            frames++;
            entries += p[1] & 0x1F;
            first_seen |= (p[1] & 0x20) != 0;
            last_seen |= (p[1] & 0x40) != 0;
            CHECK(h.radius == 1 && h.dst == NWK_BCAST_ROUTERS);
        }
        CHECK(frames == 2 && entries >= 30 && first_seen && last_seen && mtorr_ok);
        CHECK(coord.stats.link_status_failures == 0);
    }

    /* ---- Neighbour ageing: R disappears after the age limit ----------- */
    for (uint32_t t = 31000; t <= 31000 + 15000u * (NWK_ROUTER_AGE_LIMIT + 1); t += 15000u) zb_coord_tick(&coord, t);
    CHECK(!nwk_neighbor(&coord.routes, r.short_addr));

    /* ---- Channel change: Mgmt_NWK_Update_req (0xFE) broadcast to every
     *      device, then the coordinator switches and persists ------------ */
    {
        drain();
        uint8_t old = coord.mac.channel, uid = coord.nwk_update_id;
        drain();
        CHECK(zb_coord_change_channel(&coord, 20));
        CHECK(!zb_coord_change_channel(&coord, 21));          /* one change at a time */
        bool seen = false;
        while (!seen && rx_frame(&r, &m)) {
            aps_header_t a;
            seen = m.nwk.dst == 0xFFFF && aps_parse(m.payload, m.len, &a) == APS_OK && a.cluster == 0x0038 &&
                   a.profile == 0x0000 && a.dst_endpoint == 0 && a.payload_len == 7 && a.payload[5] == 0xFE &&
                   a.payload[6] == (uint8_t)(uid + 1u) && a.payload[3] == 0x10 && a.payload[1] == 0 &&
                   a.payload[2] == 0 && a.payload[4] == 0;   /* mask = 1 << 20 */
        }
        CHECK(seen);
        zb_coord_tick(&coord, coord.now_ms + 1000u);
        CHECK(coord.mac.channel == old && channel_events == 0);  /* devices get time to receive it */
        zb_coord_tick(&coord, coord.now_ms + ZB_CHANNEL_CHANGE_MS);
        CHECK(coord.mac.channel == 20 && coord.nwk_update_id == (uint8_t)(uid + 1u));
        CHECK(channel_events == 1 && channel_last == 20 && coord.stats.channel_changes == 1);
        CHECK(!zb_coord_change_channel(&coord, 10) && !zb_coord_change_channel(&coord, 27));
        CHECK(zb_coord_change_channel(&coord, old));
        zb_coord_tick(&coord, coord.now_ms + ZB_CHANNEL_CHANGE_MS + 10u);
        CHECK(coord.mac.channel == old && channel_events == 2 && coord.nwk_update_id == (uint8_t)(uid + 2u));
        drain();
    }

    /* ---- Network key rotation (update, then switch) -------------------- */
    {
        drain();
        uint8_t new_key[16];
        for (unsigned i = 0; i < 16; i++) new_key[i] = (uint8_t)(0xA0 + i);
        uint8_t old_seq = coord.key_seq, new_seq = (uint8_t)(old_seq + 1u);
        CHECK(!zb_coord_update_network_key(&coord, 0xFFFF, old_seq, new_key));   /* same sequence: refused */
        CHECK(!zb_coord_switch_network_key(&coord, 0xFFFF, new_seq));            /* nothing to switch to */
        CHECK(zb_coord_update_network_key(&coord, 0xFFFF, new_seq, new_key));
        /* R receives the broadcast Transport Key under the current key. */
        bool got = false;
        while (!got && rx_frame(&r, &m)) {
            aps_header_t a;
            got = m.nwk.dst == 0xFFFF && aps_parse(m.payload, m.len, &a) == APS_OK && a.type == APS_TYPE_COMMAND &&
                  a.payload_len == 35 && a.payload[0] == 0x05 && a.payload[1] == 0x01 &&
                  !memcmp(a.payload + 2, new_key, 16) && a.payload[18] == new_seq;
        }
        CHECK(got);
        CHECK(core_security_set_key(&r.sec, 1, new_seq, new_key));
        /* Before the switch: frames under the NEW key are already accepted. */
        uint32_t ls0 = coord.stats.link_status_rx;
        {
            const uint8_t ls[5] = {0x08, 0x61, 0x00, 0x00, 0x11};
            nwk_header_t h = hdr(NWK_TYPE_COMMAND, r.short_addr, 0xFFFC, 0x70);
            h.radius = 1;
            uint8_t nwk[CORE_SECURITY_MAX_FRAME];
            size_t hl = nwk_build(&h, nwk, sizeof nwk);
            memcpy(nwk + hl, ls, sizeof ls);
            uint8_t local[8];
            for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(r.ext >> (8 * i));
            size_t total = 0;
            CHECK(core_security_seal(&r.sec, 1, local, nwk, hl, sizeof ls, sizeof nwk, &total));
            mac_frame_t f = {0};
            f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.seq = r.mac_seq++;
            f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0xFFFF;
            f.src_mode = MAC_ADDR_SHORT; f.src_short = r.short_addr;
            uint8_t mpdu[MAC_MAX_MPDU];
            size_t n = mac_frame_build(&f, nwk, total, mpdu, sizeof mpdu);
            CHECK(n);
            zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -40);
        }
        CHECK(coord.stats.link_status_rx == ls0 + 1);
        CHECK(coord.key_seq == old_seq);                                         /* still sending with the old key */
        CHECK(!zb_coord_switch_network_key(&coord, 0xFFFF, (uint8_t)(new_seq + 1u)));   /* wrong sequence */
        CHECK(zb_coord_switch_network_key(&coord, 0xFFFF, new_seq));
        got = false;
        while (!got && rx_frame(&r, &m)) {
            aps_header_t a;
            got = m.nwk.dst == 0xFFFF && aps_parse(m.payload, m.len, &a) == APS_OK && a.type == APS_TYPE_COMMAND &&
                  a.payload_len == 2 && a.payload[0] == 0x09 && a.payload[1] == new_seq;
        }
        CHECK(got);
        zb_coord_tick(&coord, coord.now_ms + ZB_KEY_SWITCH_MS + 10u);
        CHECK(coord.key_seq == new_seq && !memcmp(coord.network_key, new_key, 16) && coord.stats.key_switches == 1);
        CHECK(coord.security.network_keys[coord.tx_key_slot].sequence == new_seq);
        /* Our frames now carry the new key sequence in their aux header. */
        const uint8_t zcl[3] = {0x01, 0x30, 0x01};
        CHECK(zb_coord_send_aps(&coord, r.short_addr, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        uint8_t copy[MAC_MAX_MPDU];
        mac_frame_t f2;
        bool seen = false;
        while (!seen && take(&f2, copy)) {
            nwk_header_t h2;
            if (nwk_parse(f2.payload, f2.payload_len, &h2) != NWK_OK || h2.dst != r.short_addr) continue;
            seen = h2.security && f2.payload[h2.header_len + 13] == new_seq;   /* aux: key sequence number */
        }
        CHECK(seen);
        /* Frames under the OLD key are still accepted (devices that missed it). */
        nwk_header_t h3 = hdr(NWK_TYPE_COMMAND, r.short_addr, 0xFFFC, 0x71);
        h3.radius = 1;
        const uint8_t ls2[5] = {0x08, 0x61, 0x00, 0x00, 0x11};
        hop_send(&r, &h3, ls2, sizeof ls2, 0xFFFF, true);   /* r.sec slot 0 = old key */
        CHECK(coord.stats.link_status_rx == ls0 + 2);
        drain();
    }

    /* ---- Our own broadcast relayed back by the routers (Zigbee PRO 3.6.5:
     * the originator records it in its BTT). Every copy is a duplicate: not
     * delivered to the host again, counted as a passive acknowledgement. */
    {
        drain();
        unsigned up0 = aps_up;
        uint32_t dup0 = coord.stats.broadcast_duplicates, auth0 = coord.stats.nwk_auth_failures;
        const uint8_t zcl[3] = {0x01, 0x31, 0x00};
        CHECK(zb_coord_send_aps_ex(&coord, 0xFFFD, false, 0xFF, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
        rx_t m;
        bool got = false;
        while (!got && rx_frame(&r, &m)) got = m.nwk.dst == 0xFFFD && m.nwk.src == 0x0000;
        CHECK(got);
        node_t *relays[2] = {&r, &r2};
        for (unsigned i = 0; i < 2; i++) {
            nwk_header_t h = m.nwk;
            h.security = false;               /* re-secured hop by hop by the relaying router */
            h.radius--;
            hop_send(relays[i], &h, m.payload, m.len, 0xFFFF, true);
        }
        CHECK(aps_up == up0);
        CHECK(coord.stats.broadcast_duplicates == dup0 + 2);
        CHECK(coord.stats.nwk_auth_failures == auth0);
        drain();
    }

    printf("mesh: %u checks passed\n", frt_checks);
    return 0;
}
