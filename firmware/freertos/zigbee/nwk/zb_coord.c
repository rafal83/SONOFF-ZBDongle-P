/* SPDX-License-Identifier: MIT
 * Zigbee spec references: 3.6.1.4 (join by association), 4.6.3.2 (Trust
 * Center authentication), 4.4.10 (APS commands: Transport/Request/Verify/
 * Confirm Key), 2.2.5.1 (APS frames and ACK), 2.4.3/2.4.4 (ZDO).
 */
#include "zb_coord.h"
#include <string.h>
#include "aps_security.h"
#include "trust_center.h"
#include "zb_ccm_backend.h"
#include "zb_mmo.h"
#include "le_buf.h"

#define STORE_ID_NETWORK 1u
#define STORE_ID_COUNTERS 2u
#define STORE_ID_DEVICE0 16u
#define STORE_NET_VERSION 2u
#define STORE_COUNTERS_VERSION 2u
#define STORE_COUNTERS_V1_LEN 8u
#define STORE_COUNTERS_V2_LEN 13u
#define STORE_NET_V1_LEN 29u
#define STORE_NET_V2_LEN 47u

#define ZDO_PROFILE 0x0000u
#define HA_PROFILE 0x0104u
#define ZDO_NWK_ADDR_REQ 0x0000u
#define ZDO_IEEE_ADDR_REQ 0x0001u
#define ZDO_NODE_DESC_REQ 0x0002u
#define ZDO_SIMPLE_DESC_REQ 0x0004u
#define ZDO_ACTIVE_EP_REQ 0x0005u
#define ZDO_MATCH_DESC_REQ 0x0006u
#define ZDO_DEVICE_ANNOUNCE 0x0013u
#define ZDO_SUCCESS 0x00u
#define ZDO_DEVICE_NOT_FOUND 0x81u
#define ZDO_INVALID_EP 0x82u
#define ZDO_NOT_SUPPORTED 0x84u

#define APS_CMD_REQUEST_KEY 0x08u
#define APS_CMD_VERIFY_KEY 0x0Fu
#define APS_CMD_CONFIRM_KEY 0x10u
#define APS_KEY_TYPE_TC_LINK 0x04u
#define APS_CMD_UPDATE_DEVICE 0x06u
#define APS_CMD_TUNNEL 0x0Eu
#define APS_CMD_SWITCH_KEY 0x09u

/* NWK commands (Zigbee PRO 3.4) */
#define NWK_CMD_ROUTE_REQUEST 0x01u
#define NWK_CMD_ROUTE_REPLY 0x02u
#define NWK_CMD_NETWORK_STATUS 0x03u
#define NWK_CMD_LEAVE 0x04u
#define NWK_CMD_ROUTE_RECORD 0x05u
#define NWK_CMD_REJOIN_REQUEST 0x06u
#define NWK_CMD_REJOIN_RESPONSE 0x07u
#define NWK_CMD_LINK_STATUS 0x08u
#define NWK_CMD_ED_TIMEOUT_REQUEST 0x0Bu
#define NWK_CMD_ED_TIMEOUT_RESPONSE 0x0Cu
#define LINK_STATUS_PERIOD_MS 15000u
#define MTORR_PERIOD_MS 120000u
#define MTORR_MIN_GAP_MS 5000u

static const zb_endpoint_t DEFAULT_EP1 = {
    .endpoint = ZB_COORD_ENDPOINT, .profile = 0x0104, .device_id = 0x0005, .version = 0,
    .in_count = 4, .in = {0x0000, 0x0003, 0x000A, 0x0019},
    .out_count = 8, .out = {0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0x0019, 0x0300},
};

static const zb_endpoint_t *find_endpoint(const zb_coord_t *c, uint8_t ep) {
    for (unsigned i = 0; i < c->endpoint_count; i++) if (c->endpoints[i].endpoint == ep) return &c->endpoints[i];
    return 0;
}

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8u * i)); }
static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---- persistence -------------------------------------------------- */
static bool store_put(zb_coord_t *c, uint16_t id, const uint8_t *data, uint16_t len) {
    if (!c->store) return true;
    if (nv_log_write(c->store, id, data, len)) { c->stats.store_writes++; return true; }
    c->stats.store_failures++;
    return false;
}

/* Fingerprint binding the counters record to the network key it protects
 * (first 4 bytes of the MMO hash: no key material is revealed). */
static uint32_t key_check(const uint8_t key[16]) {
    uint8_t d[16];
    (void)zb_mmo_hash(key, 16, d);
    uint32_t v = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    wipe(d, sizeof d);
    return v;
}

/* Counters record v2: version, NWK and TC APS reserved upper bounds, key check. */
static bool save_counters(zb_coord_t *c, uint32_t nwk_upper, uint32_t aps_upper) {
    uint8_t b[STORE_COUNTERS_V2_LEN];
    le_writer_t w = le_writer(b, sizeof b);
    le_u8(&w, STORE_COUNTERS_VERSION);
    le_u32(&w, nwk_upper);
    le_u32(&w, aps_upper);
    le_u32(&w, key_check(c->network_key));
    return store_put(c, STORE_ID_COUNTERS, b, (uint16_t)w.len);
}

static void save_network(zb_coord_t *c) {
    uint8_t b[STORE_NET_V2_LEN];
    le_writer_t w = le_writer(b, sizeof b);
    le_u8(&w, STORE_NET_VERSION);
    le_u16(&w, c->mac.pan_id);
    le_u8(&w, c->mac.channel);
    le_bytes(&w, c->mac.beacon.ext_pan_id, 8);
    le_bytes(&w, c->network_key, 16);
    le_u8(&w, c->key_seq);
    le_u8(&w, c->tclk_seed_valid ? 1u : 0u);
    le_bytes(&w, c->tclk_seed, 16);
    le_u8(&w, c->nwk_update_id);
    (void)store_put(c, STORE_ID_NETWORK, b, (uint16_t)w.len);
    wipe(b, sizeof b);
}

static void save_device(zb_coord_t *c, uint16_t slot) {
    if (!c->store || slot >= NWK_MAX_DEVICES) return;
    const nwk_device_t *d = &c->devices.dev[slot];
    uint8_t b[33];
    le_writer_t w = le_writer(b, sizeof b);
    for (unsigned i = 0; i < 8; i++) le_u8(&w, (uint8_t)(d->ext >> (8u * i)));
    le_u16(&w, d->short_addr);
    le_u8(&w, d->capability);
    le_u8(&w, (uint8_t)((d->authorized ? 1u : 0u) | (d->unique_key ? 2u : 0u) | (uint8_t)(d->relationship << 2) |
                        (d->capability_guessed ? 0x40u : 0u) | (d->aps_in_seen ? 0x80u : 0u)));
    if (d->unique_key) { le_bytes(&w, d->link_key, 16); le_u8(&w, d->tclk_shift); }
    if (d->aps_in_seen) le_u32(&w, d->aps_in_counter);   /* last authenticated incoming APS counter */
    (void)store_put(c, (uint16_t)(STORE_ID_DEVICE0 + slot), b, (uint16_t)w.len);
    wipe(b, sizeof b);
}

/* NWK outgoing counter window: durable before any frame uses it. */
static bool reserve_adapter(void *ctx, uint32_t upper) {
    zb_coord_t *c = ctx;
    bool any = false, ok = true;
    if (c->store) { any = true; ok = save_counters(c, upper, c->aps_reserved); }
    if (ok && c->upper.reserve_nwk_counter) { any = true; ok = c->upper.reserve_nwk_counter(c->upper.ctx, upper); }
    if (ok && any) c->nwk_reserved = upper;
    return ok && any;
}

/* TC APS outgoing frame counter, reserved durably like the NWK one. */
static bool next_aps_counter(zb_coord_t *c, uint32_t *out) {
    if (c->tc_aps_frame_counter == UINT32_MAX) return false;
    if (c->tc_aps_frame_counter == c->aps_reserved) {
        uint32_t room = UINT32_MAX - c->aps_reserved;
        uint32_t upper = c->aps_reserved + (room < CORE_SECURITY_RESERVATION ? room : CORE_SECURITY_RESERVATION);
        if (c->store && !save_counters(c, c->nwk_reserved, upper)) return false;
        c->aps_reserved = upper;
    }
    *out = c->tc_aps_frame_counter++;
    return true;
}

/* Current TC link key of a device: verified unique key, else the
 * preconfigured (install code) key, else the default key if allowed. */
static const uint8_t *link_key_for(zb_coord_t *c, uint64_t ext) {
    uint16_t slot = nwk_devices_find_ext(&c->devices, ext);
    if (slot != NWK_DEVICE_NONE && c->devices.dev[slot].unique_key) return c->devices.dev[slot].link_key;
    for (unsigned i = 0; i < ZB_LINK_KEYS; i++)
        if (c->link_keys[i].used && c->link_keys[i].ext == ext) return c->link_keys[i].key;
    return c->allow_default_tclk ? TC_DEFAULT_LINK_KEY : 0;
}

void zb_tclk_derive(const uint8_t seed[16], uint8_t shift, uint64_t ext, uint8_t key[16]) {
    /* herdsman adapter-backup.ts: rotated = seed[shift:] + seed[:shift];
     * key[i] = rotated[i] ^ reverse(ieee)[i % 8]. herdsman's `extAddr` getter
     * returns the address big endian, so its reverse is the little-endian
     * (over-the-air / NV) byte order used here. */
    for (unsigned i = 0; i < 16; i++) {
        uint8_t ieee_le = (uint8_t)(ext >> (8u * (i % 8u)));
        key[i] = (uint8_t)(seed[(i + shift) % 16u] ^ ieee_le);
    }
}

/* ---- transmission ---------------------------------------------------- */
typedef struct {
    uint16_t mac_dst;
    bool indirect;
    const nwk_source_route_t *source;   /* non-NULL: source-routed */
} hop_t;

static bool disc_pending_for(const zb_coord_t *c, uint16_t dst);
static bool disc_fresh_for(const zb_coord_t *c, uint16_t dst);

/* How to reach `dst`: child directly, else through a source route (Route
 * Record) or a next-hop route (Route Reply), else a neighbour router. */
static bool next_hop(zb_coord_t *c, uint16_t dst, hop_t *hop) {
    *hop = (hop_t){.mac_dst = dst};
    uint16_t slot = nwk_devices_find_short(&c->devices, dst);
    const nwk_device_t *d = slot == NWK_DEVICE_NONE ? 0 : &c->devices.dev[slot];
    if (d && d->relationship == NWK_REL_CHILD) {
        hop->indirect = !(d->capability & NWK_CAP_RX_ON_IDLE);
        return true;
    }
    const nwk_source_route_t *sr = nwk_source_route(&c->routes, dst);
    if (sr) {
        if (sr->count) { hop->mac_dst = sr->relays[sr->count - 1]; hop->source = sr; }
        return true;
    }
    uint16_t nh;
    if (nwk_table_route(&c->routes, dst, &nh)) { hop->mac_dst = nh; return true; }
    if (!disc_fresh_for(c, dst) && nwk_neighbor(&c->routes, dst)) return true;   /* sends link status: a router, always listening */
    return false;
}

/* Remember where a unicast went, to repair routes from its MAC confirm. */
static void track_tx(zb_coord_t *c, uint8_t handle, uint16_t dst, uint16_t mac_dst, uint16_t relayed_src) {
    unsigned slot = ZB_TX_TRACK;
    for (unsigned i = 0; i < ZB_TX_TRACK; i++) {
        if (c->tx_track[i].used && c->tx_track[i].handle == handle) { slot = i; break; }   /* handle wrapped */
        if (!c->tx_track[i].used && slot == ZB_TX_TRACK) slot = i;
    }
    if (slot == ZB_TX_TRACK) slot = handle % ZB_TX_TRACK;   /* full: overwrite the oldest-ish */
    c->tx_track[slot].used = true;
    c->tx_track[slot].handle = handle;
    c->tx_track[slot].dst = dst;
    c->tx_track[slot].mac_dst = mac_dst;
    c->tx_track[slot].relayed_src = relayed_src;
}

/* A handle still owned by a frame whose confirmation is pending: held during
 * route discovery, fragmented message in progress, or any MAC buffer that
 * will be confirmed (an indirect frame waits up to 7.68 s). The 8-bit counter
 * alone wrapped in ~1.4 s at 180 frames/s: two pending frames then shared a
 * handle and the host received the confirmation of the other request. */
static bool handle_in_use(const zb_coord_t *c, uint8_t handle) {
    if (c->frag_tx.used && c->frag_tx.handle == handle) return true;
    if (c->disc_count)
        for (unsigned i = 0; i < ZB_DISC_PENDING; i++)
            if (c->disc[i].used && c->disc[i].handle == handle) return true;
    for (unsigned i = 0; i < MAC_POOL_SLOTS; i++) {
        const mac_slot_t *s = &c->mac.slots[i];
        if (s->kind != SLOT_FREE && s->confirm && s->handle == handle) return true;
    }
    return false;
}

/* At most MAC_POOL_SLOTS + ZB_DISC_PENDING + 1 handles are in use: one is
 * always found within that many draws. */
static uint8_t next_mac_handle(zb_coord_t *c) {
    uint8_t h;
    unsigned guard = 0;
    do h = c->mac_handle++; while (handle_in_use(c, h) && ++guard < 256u);
    return h;
}

static uint8_t mac_send(zb_coord_t *c, uint8_t dst_mode, uint16_t dst_short, uint64_t dst_ext, bool indirect,
                        const uint8_t *frame, size_t len) {
    bool broadcast = dst_mode == MAC_ADDR_SHORT && dst_short == 0xFFFFu;
    uint8_t handle = c->forced_handle >= 0 ? (uint8_t)c->forced_handle : next_mac_handle(c);
    mac_data_request_t r = {
        .dst_mode = dst_mode, .dst_short = dst_short, .dst_ext = dst_ext, .handle = handle,
        .ack_request = !broadcast, .indirect = indirect, .priority = c->sending_ack,
        .payload = frame, .length = (uint8_t)len,
    };
    if (!c->sending_ack && !c->retrying_recent) c->last_send_handle = r.handle;
    return mac_data_request(&c->mac, &r) == MAC_STATUS_SUCCESS ? ZB_SEND_OK : ZB_SEND_MAC_REFUSED;
}

static bool is_zcl_read_request(const uint8_t *payload, size_t len) {
    if (len < 13 || (payload[0] & 0xAFu) != 0x00u) return false;
    uint16_t profile = (uint16_t)(payload[4] | (payload[5] << 8));
    if (profile != 0x0104u) return false;
    uint8_t zcl_fc = payload[8];
    if ((zcl_fc & 0x0Fu) != 0x00u) return false;
    return payload[10] == 0x00u && ((len - 11u) & 1u) == 0u;
}

static void sleepy_copies(zb_coord_t *c, const uint8_t *plain, size_t hl, size_t len);
static bool disc_hold(zb_coord_t *c, uint16_t dst, uint8_t radius, const uint8_t *payload, size_t len, bool secured,
                      bool notify_confirm);
static bool disc_suppressed(const zb_coord_t *c, uint16_t dst);
static bool answered_recently(const zb_coord_t *c, uint16_t dst);
static void bcast_queue(zb_coord_t *c, const uint8_t *plain, size_t hl, size_t len, uint16_t src, uint8_t seq,
                        uint32_t due, uint8_t sends, bool sleepy, uint8_t already_sent);

/* NWK-secure `payload` (APS frame or NWK command) and send it towards `dst`.
 * Broadcasts: one MAC broadcast. NOT done: passive-ack retransmissions and
 * indirect copies for sleepy children (0xFFFF broadcasts miss polling end
 * devices). `ext_dst` (non-zero): one-hop frame MAC-addressed to that IEEE
 * (Rejoin Response), with the NWK IEEE fields present. */
static uint8_t nwk_tx(zb_coord_t *c, uint8_t type, uint16_t dst, uint8_t radius, const uint8_t *payload, size_t len,
                      uint64_t ext_dst, bool secured, bool indirect_ext) {
    bool broadcast = nwk_is_broadcast(dst);
    hop_t hop = {.mac_dst = 0xFFFF};
    if (!broadcast && !ext_dst && !next_hop(c, dst, &hop)) {
        if (c->sending_ack) return ZB_SEND_NO_ROUTE;
        c->stats.no_route++;
        if (type == NWK_TYPE_DATA && disc_suppressed(c, dst)) {
            c->stats.rreq_suppressed++;      /* in backoff: fail at once, no flood */
            return ZB_SEND_NO_ROUTE;
        }
        if (type == NWK_TYPE_DATA) {
            /* One Route Request per destination: frames that find a
             * discovery already pending for it just wait for its reply
             * (a burst to unknown routes was sending one broadcast each). */
            if (disc_pending_for(c, dst)) c->stats.rreq_coalesced++;
            else (void)zb_coord_discover_route(c, dst);
            /* Held, not failed: sent as soon as the Route Reply arrives. */
            if (c->forced_handle < 0 && disc_hold(c, dst, radius, payload, len, secured, true)) return ZB_SEND_OK;
        }
        return ZB_SEND_NO_ROUTE;
    }
    nwk_header_t h = {0};
    h.type = type;
    h.dst = dst;
    h.src = 0x0000;
    h.radius = radius;
    if (broadcast) {
        /* The NWK sequence number advances with every frame, unicasts
         * included: under load it wraps within 1-3 s, and a broadcast reusing
         * the number of one of ours still held in the routers' broadcast
         * transaction tables (nwkBroadcastDeliveryTime) is dropped by the
         * whole network as a copy (network benchmark at 100/s: up to 90 Route
         * Requests and many-to-one requests lost per 30 s). Our own
         * broadcasts are in our BTT: their numbers are skipped. */
        for (unsigned guard = 0; guard < 256u && nwk_btt_seen(&c->routes, 0x0000, c->nwk_seq, c->now_ms); guard++) {
            c->nwk_seq++;
            c->stats.bcast_seq_skipped++;
        }
    }
    h.seq = c->nwk_seq++;
    h.discover_route = type == NWK_TYPE_DATA && !broadcast ? 1 : 0;
    if (ext_dst) { h.has_dst_ieee = true; h.dst_ieee = ext_dst; h.has_src_ieee = true; h.src_ieee = c->ext_addr; }
    /* Zigbee PRO NWK commands carry the sender's IEEE in the NWK header
     * (Link Status, Route Request incl. many-to-one, Route Reply...): real
     * routers ignored ours without it (production, 18-19 November). */
    if (type == NWK_TYPE_COMMAND) { h.has_src_ieee = true; h.src_ieee = c->ext_addr; }
    if (hop.source) {
        h.source_route = true;
        h.relay_count = hop.source->count;
        h.relay_index = (uint8_t)(hop.source->count - 1u);
        for (unsigned i = 0; i < hop.source->count; i++) h.relays[i] = hop.source->relays[i];
    }
    uint8_t frame[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(&h, frame, sizeof frame);
    if (!hl || hl + len + (secured ? 18u : 0u) > MAC_MAX_PAYLOAD) return ZB_SEND_TOO_LONG;
    for (size_t i = 0; i < len; i++) frame[hl + i] = payload[i];
    size_t total = hl + len;
    if (secured) {
        uint8_t local[8];
        put64(local, c->ext_addr);
        if (!core_security_seal(&c->security, c->tx_key_slot, local, frame, hl, len, sizeof frame, &total)) return ZB_SEND_SECURITY;
    }
    uint8_t status = ext_dst ? mac_send(c, MAC_ADDR_EXT, 0, ext_dst, indirect_ext, frame, total)
                             : mac_send(c, MAC_ADDR_SHORT, hop.mac_dst, 0, hop.indirect, frame, total);
    if (status == ZB_SEND_OK && !broadcast && !ext_dst) {
        uint8_t tracked = c->forced_handle >= 0 ? (uint8_t)c->forced_handle : c->last_send_handle;
        if (!c->sending_ack) track_tx(c, tracked, dst, hop.mac_dst, 0xFFFF);
        if (type == NWK_TYPE_DATA && !c->sending_ack && !c->retrying_recent &&
            len >= 8 && (payload[0] & 0x03u) == APS_TYPE_DATA && len <= ZB_RECENT_MAX) {
            /* One entry per outstanding READ (destination + ZCL sequence
             * number; herdsman's own retries reuse the number), one per
             * destination for other frames. Keyed by destination only, every
             * new request to a device erased the follow-up of the previous
             * one: under load (a request to each router every few hundred ms)
             * a lost response was never re-requested and cost herdsman its
             * 10 s timeout. Full table: an answered entry, else the oldest. */
            bool reply = is_zcl_read_request(payload, len);
            unsigned idx = ZB_RECENT_TX, victim = 0;
            for (unsigned i = 0; i < ZB_RECENT_TX; i++) {
                const zb_recent_tx_t *e = &c->recent_tx[i];
                if (e->used && e->dst == dst && e->expects_reply == reply && (!reply || e->payload[9] == payload[9])) {
                    idx = i;
                    break;
                }
                if (!e->used) { if (idx == ZB_RECENT_TX) idx = i; continue; }
                const zb_recent_tx_t *v = &c->recent_tx[victim];
                bool e_done = e->answered || !e->expects_reply, v_done = v->answered || !v->expects_reply;
                if (!v->used || (e_done && !v_done) ||
                    (e_done == v_done && (int32_t)(e->sent_ms - v->sent_ms) < 0))
                    victim = i;
            }
            if (idx == ZB_RECENT_TX) idx = victim;
            c->recent_tx[idx].used = true;
            c->recent_tx[idx].answered = false;
            c->recent_tx[idx].dst = dst;
            c->recent_tx[idx].radius = radius;
            c->recent_tx[idx].secured = secured;
            c->recent_tx[idx].retries = 0;
            c->recent_tx[idx].expects_reply = reply;
            c->recent_tx[idx].len = (uint8_t)len;
            c->recent_tx[idx].sent_ms = c->now_ms;
            for (size_t i = 0; i < len; i++) c->recent_tx[idx].payload[i] = payload[i];
        }
    }
    /* Originator of a broadcast: record it in the BTT (Zigbee PRO 3.6.5), so
     * the copies relayed back by the routers are duplicates (passive ACK),
     * never delivered to the host as a new frame. */
    if (broadcast && status == ZB_SEND_OK) (void)nwk_btt_accept(&c->routes, h.src, h.seq, c->now_ms);
    wipe(frame, sizeof frame);
    if (broadcast && secured && status == ZB_SEND_OK) {
        /* Rebuild the plaintext for passive-ACK retries / sleepy copies. */
        uint8_t plain[MAC_MAX_PAYLOAD];
        size_t phl = nwk_build(&h, plain, sizeof plain);
        if (phl && phl + len <= sizeof plain) {
            for (size_t i = 0; i < len; i++) plain[phl + i] = payload[i];
            if (dst == NWK_BCAST_ALL) sleepy_copies(c, plain, phl, phl + len);
            if (radius > 1) bcast_queue(c, plain, phl, phl + len, h.src, h.seq, c->now_ms + ZB_PASSIVE_ACK_MS,
                                        ZB_BCAST_RETRIES, false, 1);
        }
        wipe(plain, sizeof plain);
    }
    return status;
}

/* ---- broadcast relaying (Zigbee PRO 3.6.5) ----------------------------- */
static uint8_t send_plain(zb_coord_t *c, const uint8_t *plain, size_t hl, size_t len, uint8_t dst_mode,
                         uint16_t mac_dst, uint64_t ext, bool indirect) {
    uint8_t frame[CORE_SECURITY_MAX_FRAME];
    if (len + 18u > MAC_MAX_PAYLOAD || len > sizeof frame) return ZB_SEND_TOO_LONG;
    for (size_t i = 0; i < len; i++) frame[i] = plain[i];
    uint8_t local[8];
    put64(local, c->ext_addr);
    size_t total = 0;
    uint8_t status = ZB_SEND_SECURITY;
    if (core_security_seal(&c->security, c->tx_key_slot, local, frame, hl, len - hl, sizeof frame, &total))
        status = mac_send(c, dst_mode, mac_dst, ext, indirect, frame, total);
    wipe(frame, sizeof frame);
    return status;
}

/* Copies of a 0xFFFF broadcast for sleepy children, delivered on their polls. */
static void sleepy_copies(zb_coord_t *c, const uint8_t *plain, size_t hl, size_t len) {
    unsigned n = 0;
    for (uint16_t s = 0; s < NWK_MAX_DEVICES && n < ZB_SLEEPY_BCAST_COPIES; s++) {
        const nwk_device_t *d = &c->devices.dev[s];
        if (!d->ext || d->relationship != NWK_REL_CHILD || (d->capability & NWK_CAP_RX_ON_IDLE)) continue;
        if (send_plain(c, plain, hl, len, MAC_ADDR_SHORT, d->short_addr, 0, true) == ZB_SEND_OK) {
            c->stats.sleepy_bcast_copies++;
            n++;
        }
    }
}

static uint32_t jitter_ms(zb_coord_t *c) {
    c->devices.rng = c->devices.rng * 1103515245u + 12345u;   /* timing jitter only */
    return 2u + (c->devices.rng >> 16) % 63u;                    /* up to nwkcMaxBroadcastJitter */
}

/* Queue a plaintext NWK broadcast frame for transmission at `due`, then up
 * to `retries` passive-ACK retransmissions. */
static void bcast_queue(zb_coord_t *c, const uint8_t *plain, size_t hl, size_t len, uint16_t src, uint8_t seq,
                        uint32_t due, uint8_t sends, bool sleepy, uint8_t already_sent) {
    if (len > MAC_MAX_PAYLOAD) return;
    for (unsigned i = 0; i < ZB_BCAST_PENDING; i++) {
        if (c->bcast[i].used) continue;
        for (size_t k = 0; k < len; k++) c->bcast[i].frame[k] = plain[k];
        c->bcast[i].len = (uint8_t)len;
        c->bcast[i].hl = (uint8_t)hl;
        c->bcast[i].src = src;
        c->bcast[i].seq = seq;
        c->bcast[i].due_ms = due;
        c->bcast[i].sends_left = sends;
        c->bcast[i].heard = 0;
        c->bcast[i].sleepy_copies = sleepy;
        c->bcast[i].sent = already_sent;
        c->bcast[i].used = true;
        return;
    }
    c->stats.bcast_queue_full++;
}

/* A copy of a pending broadcast was heard from neighbour `mac_src`. */
static void bcast_heard(zb_coord_t *c, uint16_t src, uint8_t seq, uint16_t mac_src) {
    for (unsigned n = 0; n < NWK_NEIGHBORS; n++) {
        if (!c->routes.neighbors[n].used || c->routes.neighbors[n].addr != mac_src) continue;
        for (unsigned i = 0; i < ZB_BCAST_PENDING; i++)
            if (c->bcast[i].used && c->bcast[i].src == src && c->bcast[i].seq == seq) c->bcast[i].heard |= 1u << n;
    }
}

static bool all_neighbours_heard(const zb_coord_t *c, uint32_t heard) {
    for (unsigned n = 0; n < NWK_NEIGHBORS; n++)
        if (c->routes.neighbors[n].used && !(heard & (1u << n))) return false;
    return true;
}

static void bcast_service(zb_coord_t *c) {
    for (unsigned i = 0; i < ZB_BCAST_PENDING; i++) {
        if (!c->bcast[i].used || (int32_t)(c->now_ms - c->bcast[i].due_ms) < 0) continue;
        bool first = c->bcast[i].sent == 0;
        /* Done when no send is left, or (after the first send) when every
         * neighbour router was heard relaying it: passive acknowledgement. */
        if (c->bcast[i].sends_left == 0 || (!first && all_neighbours_heard(c, c->bcast[i].heard))) {
            c->bcast[i].used = false;
            continue;
        }
        (void)send_plain(c, c->bcast[i].frame, c->bcast[i].hl, c->bcast[i].len, MAC_ADDR_SHORT, 0xFFFF, 0, false);
        if (first) c->stats.rebroadcasts++;
        else c->stats.bcast_retries++;
        if (c->bcast[i].sleepy_copies) {
            sleepy_copies(c, c->bcast[i].frame, c->bcast[i].hl, c->bcast[i].len);
            c->bcast[i].sleepy_copies = false;
        }
        c->bcast[i].sent++;
        c->bcast[i].sends_left--;
        c->bcast[i].due_ms = c->now_ms + ZB_PASSIVE_ACK_MS;
    }
}

static uint8_t send_nwk(zb_coord_t *c, uint16_t dst, const uint8_t *aps, size_t aps_len) {
    return nwk_tx(c, NWK_TYPE_DATA, dst, ZB_NWK_RADIUS, aps, aps_len, 0, true, false);
}

static uint8_t send_nwk_command(zb_coord_t *c, uint16_t dst, uint8_t radius, const uint8_t *p, size_t len) {
    return nwk_tx(c, NWK_TYPE_COMMAND, dst, radius, p, len, 0, true, false);
}

static void send_network_status(zb_coord_t *c, uint16_t dst, uint8_t code, uint16_t about) {
    const uint8_t p[4] = {NWK_CMD_NETWORK_STATUS, code, (uint8_t)about, (uint8_t)(about >> 8)};
    if (send_nwk_command(c, dst, ZB_NWK_RADIUS, p, sizeof p) == ZB_SEND_OK) c->stats.network_status_tx++;
}

static void send_rejoin_response(zb_coord_t *c, uint16_t slot, uint8_t status, bool secured);

/* Two devices claim `addr` (Zigbee PRO 3.6.1.9): announce the conflict;
 * a child of ours that holds it gets a new address from us. */
static void address_conflict(zb_coord_t *c, uint16_t addr, uint64_t claimant) {
    c->stats.address_conflicts++;
    send_network_status(c, NWK_BCAST_RX_ON, 0x0D, addr);
    nwk_routes_forget(&c->routes, addr);
    uint64_t candidates[2] = {claimant, 0};
    uint16_t owner = nwk_devices_find_short(&c->devices, addr);
    if (owner != NWK_DEVICE_NONE) candidates[1] = c->devices.dev[owner].ext;
    for (unsigned i = 0; i < 2; i++) {
        uint16_t slot = candidates[i] ? nwk_devices_find_ext(&c->devices, candidates[i]) : NWK_DEVICE_NONE;
        if (slot == NWK_DEVICE_NONE || c->devices.dev[slot].relationship != NWK_REL_CHILD) continue;
        uint16_t fresh = nwk_devices_draw_address(&c->devices);
        if (!fresh || !nwk_devices_learn(&c->devices, candidates[i], fresh, NWK_REL_CHILD)) continue;
        save_device(c, slot);
        send_rejoin_response(c, slot, 0x00, true);   /* unsolicited: "your address is now ..." */
        c->stats.addresses_reassigned++;
    }
}

bool zb_coord_discover_route(zb_coord_t *c, uint16_t dst) {
    if (nwk_is_broadcast(dst) || dst == 0x0000) return false;
    if (disc_suppressed(c, dst)) { c->stats.rreq_suppressed++; return true; }   /* accepted, not flooded */
    const uint8_t rreq[6] = {NWK_CMD_ROUTE_REQUEST, 0x00, ++c->routes.rreq_id, (uint8_t)dst, (uint8_t)(dst >> 8), 0};
    bool ok = send_nwk_command(c, NWK_BCAST_ROUTERS, ZB_NWK_RADIUS, rreq, sizeof rreq) == ZB_SEND_OK;
    if (ok) c->stats.rreq_tx++;
    return ok;
}

/* ---- frames held during route discovery ------------------------------ */
static bool disc_hold(zb_coord_t *c, uint16_t dst, uint8_t radius, const uint8_t *payload, size_t len, bool secured,
                      bool notify_confirm) {
    if (!c->mac.started || len > MAC_MAX_PAYLOAD || dst == 0x0000) return false;   /* no network: fail now */
    for (unsigned i = 0; i < ZB_DISC_PENDING; i++) {
        if (c->disc[i].used) continue;
        c->disc[i].handle = next_mac_handle(c);   /* reserved until sent or expired */
        c->disc[i].used = true;
        c->disc[i].dst = dst;
        c->disc[i].radius = radius;
        c->disc[i].secured = secured;
        c->disc[i].notify_confirm = notify_confirm;
        c->disc[i].len = (uint8_t)len;
        for (size_t k = 0; k < len; k++) c->disc[i].payload[k] = payload[k];
        c->disc[i].due_ms = c->now_ms + ZB_DISC_WAIT_MS;
        c->disc_count++;
        if (notify_confirm) c->last_send_handle = c->disc[i].handle;
        c->stats.disc_buffered++;
        return true;
    }
    return false;
}

/* A frame already waits for a route to `dst`, its Route Request sent less
 * than ZB_DISC_WAIT_MS ago. */
static bool disc_pending_for(const zb_coord_t *c, uint16_t dst) {
    if (!c->disc_count) return false;
    for (unsigned i = 0; i < ZB_DISC_PENDING; i++)
        if (c->disc[i].used && c->disc[i].dst == dst && (int32_t)(c->now_ms - c->disc[i].due_ms) < 0) return true;
    return false;
}

static bool disc_fresh_for(const zb_coord_t *c, uint16_t dst) {
    if (!c->disc_count) return false;
    for (unsigned i = 0; i < ZB_DISC_PENDING; i++)
        if (c->disc[i].used && c->disc[i].dst == dst &&
            (int32_t)(c->disc[i].due_ms - c->now_ms) > (int32_t)(ZB_DISC_WAIT_MS - 1000u))
            return true;
    return false;
}

/* ---- route discovery backoff (see ZB_RDISC_FAIL) ----------------------- */
static bool disc_suppressed(const zb_coord_t *c, uint16_t dst) {
    for (unsigned i = 0; i < ZB_RDISC_FAIL; i++)
        if (c->rdisc_fail[i].used && c->rdisc_fail[i].dst == dst && (int32_t)(c->now_ms - c->rdisc_fail[i].until_ms) < 0)
            return true;
    return false;
}

/* The discovery for `dst` got no reply: back off (doubling after each new
 * failure, once the previous backoff is over). */
static void disc_failed(zb_coord_t *c, uint16_t dst) {
    unsigned slot = ZB_RDISC_FAIL, oldest = 0;
    for (unsigned i = 0; i < ZB_RDISC_FAIL; i++) {
        if (c->rdisc_fail[i].used && c->rdisc_fail[i].dst == dst) { slot = i; break; }
        if (!c->rdisc_fail[i].used) { if (slot == ZB_RDISC_FAIL) slot = i; continue; }
        if ((int32_t)(c->rdisc_fail[i].until_ms - c->rdisc_fail[oldest].until_ms) < 0) oldest = i;
    }
    if (slot == ZB_RDISC_FAIL) slot = oldest;
    if (c->rdisc_fail[slot].used && c->rdisc_fail[slot].dst == dst) {
        if ((int32_t)(c->now_ms - c->rdisc_fail[slot].until_ms) < 0) return;   /* already backing off */
        uint32_t b = c->rdisc_fail[slot].backoff_ms * 2u;
        c->rdisc_fail[slot].backoff_ms = b > ZB_RDISC_BACKOFF_MAX_MS ? ZB_RDISC_BACKOFF_MAX_MS : b;
    } else {
        c->rdisc_fail[slot].used = true;
        c->rdisc_fail[slot].dst = dst;
        c->rdisc_fail[slot].backoff_ms = ZB_RDISC_BACKOFF_MS;
    }
    c->rdisc_fail[slot].until_ms = c->now_ms + c->rdisc_fail[slot].backoff_ms;
}

/* `addr` was heard (authenticated frame): it is reachable again. */
static void disc_heard(zb_coord_t *c, uint16_t addr) {
    for (unsigned i = 0; i < ZB_RDISC_FAIL; i++)
        if (c->rdisc_fail[i].used && c->rdisc_fail[i].dst == addr) c->rdisc_fail[i].used = false;
}

static void disc_release(zb_coord_t *c, unsigned i) {
    wipe(c->disc[i].payload, sizeof c->disc[i].payload);
    c->disc[i].used = false;
    c->disc_count--;
}

/* Send what has a route now; confirm NWK_NO_ROUTE what waited too long. */
static void disc_service(zb_coord_t *c) {
    if (!c->disc_count) return;
    for (unsigned i = 0; i < ZB_DISC_PENDING; i++) {
        if (!c->disc[i].used) continue;
        hop_t hop = {.mac_dst = 0xFFFF};
        uint8_t handle = c->disc[i].handle;
        bool notify = c->disc[i].notify_confirm;
        if (next_hop(c, c->disc[i].dst, &hop)) {
            uint8_t frame[MAC_MAX_PAYLOAD];
            uint8_t len = c->disc[i].len, radius = c->disc[i].radius;
            uint16_t dst = c->disc[i].dst;
            bool secured = c->disc[i].secured;
            for (unsigned k = 0; k < len; k++) frame[k] = c->disc[i].payload[k];
            disc_release(c, i);
            c->forced_handle = handle;
            c->retrying_recent = !notify;
            uint8_t st = nwk_tx(c, NWK_TYPE_DATA, dst, radius, frame, len, 0, secured, false);
            c->retrying_recent = false;
            c->forced_handle = -1;
            wipe(frame, sizeof frame);
            if (st == ZB_SEND_OK) {
                c->stats.disc_sent++;
            } else if (notify && c->upper.send_confirm) {
                c->upper.send_confirm(c->upper.ctx, handle, st == ZB_SEND_MAC_REFUSED ? ZB_STATUS_BUFFER_FULL
                                                                                    : ZB_STATUS_NWK_NO_ROUTE);
            }
        } else if ((int32_t)(c->now_ms - c->disc[i].due_ms) >= 0) {
            if (notify && !answered_recently(c, c->disc[i].dst)) disc_failed(c, c->disc[i].dst);
            disc_release(c, i);
            c->stats.disc_expired++;
            if (notify && c->upper.send_confirm) c->upper.send_confirm(c->upper.ctx, handle, ZB_STATUS_NWK_NO_ROUTE);
        }
    }
}

static void note_answer(zb_coord_t *c, uint16_t dst) {
    unsigned slot = c->answered_next % ZB_ANSWERED;
    for (unsigned i = 0; i < ZB_ANSWERED; i++)
        if (c->answered[i].dst == dst && c->answered[i].ms) { slot = i; break; }
    if (slot == c->answered_next % ZB_ANSWERED) c->answered_next++;
    c->answered[slot].dst = dst;
    c->answered[slot].ms = c->now_ms | 1u;   /* never 0: 0 marks an empty entry */
}

static bool answered_recently(const zb_coord_t *c, uint16_t dst) {
    for (unsigned i = 0; i < ZB_ANSWERED; i++)
        if (c->answered[i].ms && c->answered[i].dst == dst && c->now_ms - c->answered[i].ms < ZB_ANSWERED_MS) return true;
    return false;
}

static bool tx_in_flight_for(const zb_coord_t *c, uint16_t dst) {
    for (unsigned i = 0; i < ZB_TX_TRACK; i++)
        if (c->tx_track[i].used && c->tx_track[i].dst == dst) return true;
    return false;
}

/* Known not to listen while idle (announce, rejoin, association, poll or
 * Node Descriptor): it fetches its frames from its parent when it polls,
 * seconds later, so a missing response after 1.1 s says nothing about the
 * route. Re-sending would queue a second copy at the parent (answered twice)
 * and the route rebuild would flood a Route Request for a working route. */
static bool known_sleepy(zb_coord_t *c, uint16_t addr) {
    uint16_t s = nwk_devices_find_short(&c->devices, addr);
    if (s == NWK_DEVICE_NONE) return false;
    const nwk_device_t *d = &c->devices.dev[s];
    return !d->capability_guessed && !(d->capability & NWK_CAP_RX_ON_IDLE);
}

static void recent_service(zb_coord_t *c) {
    /* refill the re-send budget */
    uint32_t elapsed = c->now_ms - c->retx_refill_ms;
    if (elapsed >= ZB_RETX_TOKEN_MS) {
        uint32_t add = elapsed / ZB_RETX_TOKEN_MS;
        c->retx_tokens = (uint8_t)(c->retx_tokens + add > ZB_RETX_BURST ? ZB_RETX_BURST : c->retx_tokens + add);
        c->retx_refill_ms += add * ZB_RETX_TOKEN_MS;
    }
    for (unsigned i = 0; i < ZB_RECENT_TX; i++) {
        if (!c->recent_tx[i].used) continue;
        int32_t age = (int32_t)(c->now_ms - c->recent_tx[i].sent_ms);
        if (age > 4500) {
            c->recent_tx[i].used = false;
            continue;
        }
        if (!c->recent_tx[i].expects_reply || c->recent_tx[i].answered) continue;
        uint16_t dst = c->recent_tx[i].dst;
        if (disc_pending_for(c, dst) || tx_in_flight_for(c, dst)) continue;
        if (known_sleepy(c, dst)) {
            c->recent_tx[i].used = false;
            c->stats.zcl_retx_sleepy_skipped++;
            continue;
        }
        uint8_t r = c->recent_tx[i].retries;
        uint32_t due = r == 0 ? ZB_RECENT_RETRY1_MS : r == 1 ? ZB_RECENT_RETRY2_MS : ZB_RECENT_RETRY3_MS;
        if (r >= 3 || age < (int32_t)due) continue;
        if (!c->retx_tokens) { c->stats.zcl_retx_deferred++; continue; }   /* budget: retried later or left to the host */
        c->recent_tx[i].retries = (uint8_t)(r + 1);
        c->retx_tokens--;
        c->recent_tx[i].sent_ms = c->now_ms;
        c->recent_tx[i].payload[7] = c->aps_counter++;
        if (r == 1 && !answered_recently(c, dst)) {
            /* Second miss and nothing answered lately: the route itself may
             * be broken (asymmetric link, relay gone): rebuild it. A
             * destination that answered other reads within ZB_ANSWERED_MS
             * has a working route; its lost answer was congestion, and the
             * rebuild (route forgotten, Route Request flood, held frames)
             * only made it worse: under load, a lost Route Reply then banned
             * reachable routers for 30 s (network benchmark at 100/s). */
            nwk_routes_forget(&c->routes, dst);
            disc_heard(c, dst);
            if (!disc_pending_for(c, dst)) (void)zb_coord_discover_route(c, dst);
            if (disc_hold(c, dst, c->recent_tx[i].radius, c->recent_tx[i].payload, c->recent_tx[i].len,
                          c->recent_tx[i].secured, false))
                c->stats.net_status_retries++;
            c->stats.zcl_route_rebuilds++;
            continue;
        }
        c->retrying_recent = true;
        c->forced_handle = next_mac_handle(c);
        uint8_t st = nwk_tx(c, NWK_TYPE_DATA, dst, c->recent_tx[i].radius, c->recent_tx[i].payload,
                            c->recent_tx[i].len, 0, c->recent_tx[i].secured, false);
        c->forced_handle = -1;
        c->retrying_recent = false;
        if (st == ZB_SEND_OK) c->stats.net_status_retries++;
    }
}

static void send_mtorr(zb_coord_t *c) {
    /* Many-to-one, route record table required (options bits 3-4 = 01). */
    const uint8_t rreq[6] = {NWK_CMD_ROUTE_REQUEST, 0x08, ++c->routes.rreq_id, 0xFC, 0xFF, 0};
    if (send_nwk_command(c, NWK_BCAST_ROUTERS, ZB_NWK_RADIUS, rreq, sizeof rreq) == ZB_SEND_OK) c->stats.mtorr_tx++;
    c->last_mtorr_ms = c->now_ms;
}

/* Link Status, split so that each frame fits: NWK header with IEEE (16) +
 * aux (14) + MIC (4) leaves 70 bytes, i.e. 22 entries, under MAC_MAX_PAYLOAD.
 * (A single frame for more than ~25 neighbours was too long and never sent.) */
#define LINK_STATUS_ENTRIES_PER_FRAME 20u
static void send_link_status(zb_coord_t *c) {
    uint8_t p[2 + 3 * LINK_STATUS_ENTRIES_PER_FRAME];
    unsigned start = 0, next = 0;
    for (unsigned frame = 0; frame < 4; frame++) {
        size_t n = nwk_link_status_chunk(&c->routes, start, LINK_STATUS_ENTRIES_PER_FRAME, p, sizeof p, &next);
        if (!n) return;
        if (send_nwk_command(c, NWK_BCAST_ROUTERS, 1, p, n) == ZB_SEND_OK) c->stats.link_status_tx++;
        else c->stats.link_status_failures++;
        if (p[1] & 0x40u) return;   /* last frame */
        start = next;
    }
}

/* APS command, APS-secured with `key` under key identifier `key_id`. */
static bool aps_fresh(zb_coord_t *c, uint16_t src, uint8_t counter, uint16_t cluster, const uint8_t *data, size_t len);
static void send_aps_ack(zb_coord_t *c, const nwk_header_t *h, const aps_header_t *a, const uint8_t *key);
/* ---- APS fragmentation (window of one block) ---------------------------- */
static void frag_tx_finish(zb_coord_t *c, uint8_t status) {
    zb_frag_tx_t *t = &c->frag_tx;
    uint8_t handle = t->handle;
    wipe(t->data, sizeof t->data);
    t->used = t->waiting = false;
    if (status == MAC_STATUS_SUCCESS) c->stats.frag_tx_ok++;
    else c->stats.frag_tx_failed++;
    if (c->upper.send_confirm) c->upper.send_confirm(c->upper.ctx, handle, status);
}

static uint8_t frag_tx_block(zb_coord_t *c) {
    zb_frag_tx_t *t = &c->frag_tx;
    aps_header_t a = {0};
    a.type = APS_TYPE_DATA;
    a.delivery = APS_DELIVERY_UNICAST;
    a.ack_request = true;
    a.extended = true;
    a.fragmentation = t->block == 0 ? APS_FRAG_FIRST : APS_FRAG_NEXT;
    a.block_number = t->block == 0 ? t->total : t->block;   /* first block: number of blocks */
    a.dst_endpoint = t->dst_ep;
    a.src_endpoint = t->src_ep;
    a.cluster = t->cluster;
    a.profile = t->profile;
    a.counter = t->counter;
    uint8_t aps[MAC_MAX_PAYLOAD];
    size_t hl = aps_build(&a, aps, sizeof aps);
    unsigned off = (unsigned)t->block * ZB_FRAG_BLOCK;
    unsigned n = t->len - off < ZB_FRAG_BLOCK ? t->len - off : ZB_FRAG_BLOCK;
    if (!hl || hl + n > sizeof aps) return ZB_SEND_TOO_LONG;
    for (unsigned k = 0; k < n; k++) aps[hl + k] = t->data[off + k];
    uint8_t status = send_nwk(c, t->dst, aps, hl + n);
    t->waiting = status == ZB_SEND_OK;
    t->due_ms = c->now_ms + ZB_FRAG_WAIT_MS;
    return status;
}

uint8_t zb_coord_send_aps_fragmented(zb_coord_t *c, uint16_t dst, uint8_t dst_ep, uint8_t src_ep, uint16_t cluster,
                                     uint16_t profile, const uint8_t *payload, uint16_t length) {
    zb_frag_tx_t *t = &c->frag_tx;
    if (length > ZB_FRAG_MAX || nwk_is_broadcast(dst)) return ZB_SEND_TOO_LONG;
    if (t->used) return ZB_SEND_MAC_REFUSED;   /* one at a time: the host retries */
    *t = (zb_frag_tx_t){0};
    t->used = true;
    t->dst = dst;
    t->dst_ep = dst_ep;
    t->src_ep = src_ep;
    t->cluster = cluster;
    t->profile = profile;
    t->len = length;
    t->total = (uint8_t)((length + ZB_FRAG_BLOCK - 1u) / ZB_FRAG_BLOCK);
    t->counter = c->aps_counter++;
    for (uint16_t k = 0; k < length; k++) t->data[k] = payload[k];
    t->handle = next_mac_handle(c);   /* confirmed to the host once, at the end */
    uint8_t status = frag_tx_block(c);
    if (status != ZB_SEND_OK) {
        wipe(t->data, sizeof t->data);
        t->used = false;
        c->stats.aps_tx_failures++;
        return status;
    }
    c->last_send_handle = t->handle;
    c->stats.aps_tx++;
    return ZB_SEND_OK;
}

/* APS ACK of the block in flight: next block, or the whole message done. */
static void frag_ack(zb_coord_t *c, uint16_t src, const aps_header_t *a) {
    zb_frag_tx_t *t = &c->frag_tx;
    if (!t->used || !t->waiting || src != t->dst || a->counter != t->counter) return;
    if (!a->extended || a->fragmentation == APS_FRAG_NONE || a->block_number != t->block || !(a->ack_bitfield & 1u))
        return;
    t->waiting = false;
    t->retries = 0;
    if (++t->block >= t->total) { frag_tx_finish(c, MAC_STATUS_SUCCESS); return; }
    uint8_t st = frag_tx_block(c);
    if (st != ZB_SEND_OK) frag_tx_finish(c, st == ZB_SEND_NO_ROUTE ? ZB_STATUS_NWK_NO_ROUTE : ZB_STATUS_APS_NO_ACK);
}

/* One block received: in order, acknowledged, delivered once complete. */
static void frag_rx(zb_coord_t *c, const nwk_header_t *h, const aps_header_t *a, const uint8_t *key, uint8_t lqi) {
    zb_frag_rx_t *s = 0;
    for (unsigned i = 0; i < ZB_FRAG_RX_SESSIONS; i++)
        if (c->frag_rx[i].used && c->frag_rx[i].src == h->src && c->frag_rx[i].counter == a->counter) s = &c->frag_rx[i];
    if (a->fragmentation == APS_FRAG_FIRST) {
        if (a->block_number == 0 || a->block_number > ZB_FRAG_MAX_BLOCKS) { c->stats.frag_rx_dropped++; return; }
        if (!s) {
            for (unsigned i = 0; i < ZB_FRAG_RX_SESSIONS && !s; i++) if (!c->frag_rx[i].used) s = &c->frag_rx[i];
            if (!s) {   /* replace the oldest session */
                s = &c->frag_rx[0];
                for (unsigned i = 1; i < ZB_FRAG_RX_SESSIONS; i++)
                    if ((int32_t)(c->frag_rx[i].due_ms - s->due_ms) < 0) s = &c->frag_rx[i];
                c->stats.frag_rx_dropped++;
            }
            *s = (zb_frag_rx_t){.used = true, .src = h->src, .counter = a->counter, .total = a->block_number,
                                .dst_ep = a->dst_endpoint, .src_ep = a->src_endpoint, .cluster = a->cluster,
                                .profile = a->profile};
        }
        if (s->next == 0) {
            if (a->payload_len > ZB_FRAG_MAX) { s->used = false; c->stats.frag_rx_dropped++; return; }
            for (size_t k = 0; k < a->payload_len; k++) s->data[k] = a->payload[k];
            s->len = (uint16_t)a->payload_len;
            s->next = 1;
        }
    } else {
        if (!s) return;   /* subsequent block of an unknown message: no ACK */
        if (a->block_number == s->next) {
            if (s->len + a->payload_len > ZB_FRAG_MAX) { s->used = false; c->stats.frag_rx_dropped++; return; }
            for (size_t k = 0; k < a->payload_len; k++) s->data[s->len + k] = a->payload[k];
            s->len = (uint16_t)(s->len + a->payload_len);
            s->next++;
        } else if (a->block_number > s->next) {
            return;   /* gap: the sender repeats the missing block (window of one) */
        }
    }
    s->due_ms = c->now_ms + ZB_FRAG_WAIT_MS;
    s->lqi = lqi;
    if (a->ack_request) send_aps_ack(c, h, a, key);
    if (s->next < s->total) return;
    /* Complete: delivered exactly once (the duplicate filter sees it now). */
    aps_header_t whole = *a;
    whole.extended = false;
    whole.fragmentation = APS_FRAG_NONE;
    s->used = false;
    if (!aps_fresh(c, h->src, s->counter, a->cluster, s->data, s->len)) { c->stats.aps_duplicates++; return; }
    c->stats.frag_rx_ok++;
    if (c->upper.aps_data) {
        c->stats.aps_data_up++;
        c->upper.aps_data(c->upper.ctx, h, &whole, s->data, s->len, s->lqi);
    }
}

static void frag_service(zb_coord_t *c) {
    zb_frag_tx_t *t = &c->frag_tx;
    if (t->used && t->waiting && (int32_t)(c->now_ms - t->due_ms) >= 0) {
        if (t->retries++ < ZB_FRAG_RETRIES) {
            c->stats.frag_tx_retries++;
            uint8_t st = frag_tx_block(c);
            if (st != ZB_SEND_OK && st != ZB_SEND_MAC_REFUSED) frag_tx_finish(c, ZB_STATUS_APS_NO_ACK);
            else if (st == ZB_SEND_MAC_REFUSED) { t->waiting = true; t->due_ms = c->now_ms + 200u; }
        } else {
            frag_tx_finish(c, ZB_STATUS_APS_NO_ACK);
        }
    }
    for (unsigned i = 0; i < ZB_FRAG_RX_SESSIONS; i++)
        if (c->frag_rx[i].used && (int32_t)(c->now_ms - c->frag_rx[i].due_ms) >= 0) {
            c->frag_rx[i].used = false;
            c->stats.frag_rx_dropped++;
        }
}

static uint8_t send_aps_command(zb_coord_t *c, uint16_t dst, const uint8_t key[16], uint8_t key_id,
                                const uint8_t *payload, size_t len) {
    const uint8_t hdr[2] = {0x01 /* command, unicast */, c->aps_counter++};
    uint32_t fc;
    if (!next_aps_counter(c, &fc)) return ZB_SEND_SECURITY;
    nwk_aux_t aux = {.key_id = key_id, .extended_nonce = true, .frame_counter = fc, .source = c->ext_addr};
    uint8_t aps[MAC_MAX_PAYLOAD];
    size_t n = aps_seal(key, hdr, sizeof hdr, &aux, payload, len, aps, sizeof aps);
    uint8_t status = n ? send_nwk(c, dst, aps, n) : ZB_SEND_SECURITY;
    wipe(aps, sizeof aps);
    return status;
}

uint8_t zb_coord_send_aps(zb_coord_t *c, uint16_t dst, uint8_t dst_ep, uint8_t src_ep, uint16_t cluster,
                          uint16_t profile, const uint8_t *payload, uint8_t length, bool ack_request) {
    return zb_coord_send_aps_ex(c, dst, false, dst_ep, src_ep, cluster, profile, payload, length, ack_request);
}

uint8_t zb_coord_send_aps_ex(zb_coord_t *c, uint16_t dst, bool group, uint8_t dst_ep, uint8_t src_ep,
                             uint16_t cluster, uint16_t profile, const uint8_t *payload, uint8_t length,
                             bool ack_request) {
    aps_header_t a = {0};
    a.type = APS_TYPE_DATA;
    bool broadcast = group || nwk_is_broadcast(dst);
    a.delivery = group ? APS_DELIVERY_GROUP : broadcast ? APS_DELIVERY_BROADCAST : APS_DELIVERY_UNICAST;
    a.group = group ? dst : 0;
    if (group) dst = 0xFFFDu;   /* NWK broadcast to rx-on-when-idle devices */
    a.ack_request = ack_request && !broadcast;
    a.dst_endpoint = dst_ep;
    a.src_endpoint = src_ep;
    a.cluster = cluster;
    a.profile = profile;
    if (!broadcast && length > ZB_UNFRAG_MAX)
        return zb_coord_send_aps_fragmented(c, dst, dst_ep, src_ep, cluster, profile, payload, length);
    a.counter = c->aps_counter++;
    uint8_t aps[MAC_MAX_PAYLOAD];
    size_t hl = aps_build(&a, aps, sizeof aps);
    if (!hl || hl + length > sizeof aps) { c->stats.aps_tx_failures++; return ZB_SEND_TOO_LONG; }
    for (uint8_t i = 0; i < length; i++) aps[hl + i] = payload[i];
    uint8_t status = send_nwk(c, dst, aps, hl + length);
    if (status == ZB_SEND_OK) c->stats.aps_tx++;
    else c->stats.aps_tx_failures++;
    return status;
}

uint8_t zb_coord_send_aps_secured(zb_coord_t *c, uint16_t dst, uint8_t dst_ep, uint8_t src_ep, uint16_t cluster,
                                  uint16_t profile, const uint8_t *payload, uint8_t length, bool ack_request) {
    uint16_t slot = nwk_devices_find_short(&c->devices, dst);
    if (nwk_is_broadcast(dst) || slot == NWK_DEVICE_NONE) return ZB_SEND_NO_ROUTE;
    const uint8_t *key = link_key_for(c, c->devices.dev[slot].ext);
    uint32_t fc;
    if (!key || key == TC_DEFAULT_LINK_KEY || !next_aps_counter(c, &fc)) return ZB_SEND_SECURITY;
    aps_header_t a = {0};
    a.type = APS_TYPE_DATA;
    a.delivery = APS_DELIVERY_UNICAST;
    a.ack_request = ack_request;
    a.dst_endpoint = dst_ep;
    a.src_endpoint = src_ep;
    a.cluster = cluster;
    a.profile = profile;
    a.counter = c->aps_counter++;
    uint8_t hdr[16];
    size_t hl = aps_build(&a, hdr, sizeof hdr);
    if (!hl) return ZB_SEND_TOO_LONG;
    nwk_aux_t aux = {.key_id = APS_KEY_DATA, .extended_nonce = true, .frame_counter = fc, .source = c->ext_addr};
    uint8_t aps[MAC_MAX_PAYLOAD];
    size_t n = aps_seal(key, hdr, hl, &aux, payload, length, aps, sizeof aps);
    uint8_t status = n ? send_nwk(c, dst, aps, n) : ZB_SEND_TOO_LONG;
    wipe(aps, sizeof aps);
    if (status == ZB_SEND_OK) { c->stats.aps_tx++; c->stats.aps_secured_tx++; }
    else c->stats.aps_tx_failures++;
    return status;
}

/* APS duplicate rejection (Zigbee 2.2.8.4.2): true when (src, counter) is new. */
/* APS duplicate rejection. A duplicate is a RETRANSMISSION: same source,
 * same APS counter AND same content (cluster + payload, 16-bit FNV-1a).
 * Some devices reuse their APS counter for different frames (e.g. the
 * response to a request right after a report): with the counter alone, such
 * a fresh response was dropped as a duplicate. */
static uint16_t aps_hash(uint16_t cluster, const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    h = (h ^ (cluster & 0xFFu)) * 16777619u;
    h = (h ^ (cluster >> 8)) * 16777619u;
    for (size_t i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
    return (uint16_t)(h ^ (h >> 16));
}

static bool aps_fresh(zb_coord_t *c, uint16_t src, uint8_t counter, uint16_t cluster, const uint8_t *data, size_t len) {
    uint16_t hash = aps_hash(cluster, data, len);
    bool reuse = false;
    for (unsigned i = 0; i < ZB_APS_DUP_TABLE; i++) {
        if (!c->aps_dup[i].used || c->now_ms - c->aps_dup[i].ms > ZB_APS_DUP_MS) continue;
        if (c->aps_dup[i].src != src || c->aps_dup[i].counter != counter) continue;
        if (c->aps_dup[i].hash == hash) return false;
        reuse = true;
    }
    if (reuse) c->stats.aps_counter_reuse++;
    unsigned i = c->aps_dup_next++ % ZB_APS_DUP_TABLE;
    c->aps_dup[i].used = true;
    c->aps_dup[i].src = src;
    c->aps_dup[i].counter = counter;
    c->aps_dup[i].hash = hash;
    c->aps_dup[i].ms = c->now_ms;
    return true;
}

/* ---- MAC upper callbacks --------------------------------------------- */
static void on_assoc(void *ctx, uint64_t device, uint8_t capability, uint8_t lqi) {
    zb_coord_t *c = ctx;
    (void)lqi;
    uint16_t short_addr;
    uint8_t status = NWK_ASSOC_ACCESS_DENIED;
    /* A device without any usable link key could never decrypt the network
     * key: refuse it at association instead of leaving it half joined.
     * A successful (re)association resets the unique key inside
     * nwk_devices_associate(); a refused one must leave it untouched, or a
     * spoofed association request could wipe a device's key. */
    if (link_key_for(c, device))
        status = nwk_devices_associate(&c->devices, device, capability, c->permit, c->now_ms, &short_addr);
    if (status != NWK_ASSOC_SUCCESS) short_addr = 0xFFFF;
    if (mac_association_response(&c->mac, device, short_addr, status) != MAC_STATUS_SUCCESS) return;
    if (status != NWK_ASSOC_SUCCESS) { c->stats.associations_denied++; return; }
    c->stats.associations_accepted++;
    save_device(c, nwk_devices_find_ext(&c->devices, device));
    for (unsigned i = 0; i < ZB_AWAITING_KEY; i++) if (c->awaiting_key[i] == device) return;
    for (unsigned i = 0; i < ZB_AWAITING_KEY; i++) {
        if (!c->awaiting_key[i]) { c->awaiting_key[i] = device; return; }
    }
    c->stats.transport_key_failures++;   /* table full: device will time out and retry */
}

static void send_transport_key(zb_coord_t *c, uint16_t slot) {
    const nwk_device_t *d = &c->devices.dev[slot];
    const uint8_t *tclk = link_key_for(c, d->ext);
    uint32_t fc;
    if (!tclk || !next_aps_counter(c, &fc)) { c->stats.transport_key_failures++; return; }
    tc_transport_key_t p = {
        .tc_link_key = tclk, .network_key = c->network_key, .key_seq = c->key_seq,
        .dst_short = d->short_addr, .dst_ext = d->ext, .tc_ext = c->ext_addr,
        .nwk_seq = c->nwk_seq++, .radius = ZB_NWK_RADIUS, .aps_counter = c->aps_counter++,
        .aps_frame_counter = fc,
    };
    uint8_t frame[MAC_MAX_PAYLOAD];
    size_t n = tc_build_transport_network_key(&p, frame, sizeof frame);
    if (!n) { c->stats.transport_key_failures++; return; }
    mac_data_request_t r = {
        .dst_mode = MAC_ADDR_SHORT, .dst_short = d->short_addr, .handle = next_mac_handle(c),
        .ack_request = true, .indirect = !(d->capability & NWK_CAP_RX_ON_IDLE),
        .payload = frame, .length = (uint8_t)n,
    };
    if (mac_data_request(&c->mac, &r) == MAC_STATUS_SUCCESS) c->stats.transport_keys_sent++;
    else c->stats.transport_key_failures++;
    wipe(frame, n);
}

/* A device that polls us is a sleepy child: deliver to it indirectly. */
static void on_poll(void *ctx, uint16_t short_addr, uint64_t ext, bool served) {
    zb_coord_t *c = ctx;
    (void)served;
    uint16_t slot = short_addr < 0xFFF8u ? nwk_devices_find_short(&c->devices, short_addr) : NWK_DEVICE_NONE;
    if (slot == NWK_DEVICE_NONE && ext) slot = nwk_devices_find_ext(&c->devices, ext);
    if (slot == NWK_DEVICE_NONE) return;
    nwk_device_t *d = &c->devices.dev[slot];
    /* Polling with its SHORT address, an end device has us as its parent:
     * it is our child even if we had it behind a router (e.g. the first copy
     * we heard of its Device Announce was relayed, or it moved back to us
     * without a rejoin we saw). Otherwise its frames went to a route that no
     * longer leads to it, until it rejoined. Known routers never poll. */
    if (short_addr < 0xFFF8u && d->relationship != NWK_REL_CHILD &&
        (d->capability_guessed || !(d->capability & NWK_CAP_FFD))) {
        d->relationship = NWK_REL_CHILD;
        c->stats.poll_children++;
        if (!d->capability_guessed) { save_device(c, slot); return; }
    }
    /* Every joiner polls once for its association response: only a device
     * whose capability is unknown (imported) is classified by polling. */
    if (!d->capability_guessed) return;
    d->capability_guessed = false;
    d->capability &= (uint8_t)~NWK_CAP_RX_ON_IDLE;
    d->relationship = NWK_REL_CHILD;
    save_device(c, slot);
}

static void send_network_status(zb_coord_t *c, uint16_t dst, uint8_t code, uint16_t about);
static void send_mtorr(zb_coord_t *c);

/* Many-to-one request outside the periodic schedule. Within `min_gap` of
 * the previous one it is DEFERRED to the end of the gap instead of being
 * dropped: the previous request may predate the change (e.g. routers that
 * were still off), and waiting for the periodic one costs up to 2 minutes. */
static void early_mtorr(zb_coord_t *c, uint32_t min_gap) {
    if (c->now_ms - c->last_mtorr_ms > min_gap) {
        send_mtorr(c);
        c->stats.early_mtorr++;
        return;
    }
    uint32_t at = c->last_mtorr_ms + min_gap + 1u;
    if ((int32_t)(c->next_mtorr_ms - at) > 0) {
        c->next_mtorr_ms = at;
        c->stats.early_mtorr_deferred++;
    }
}

/* Route repair from the MAC outcome of a tracked unicast. */
static void route_outcome(zb_coord_t *c, uint8_t handle, uint8_t status) {
    for (unsigned i = 0; i < ZB_TX_TRACK; i++) {
        if (!c->tx_track[i].used || c->tx_track[i].handle != handle) continue;
        c->tx_track[i].used = false;
        uint16_t dst = c->tx_track[i].dst, hop = c->tx_track[i].mac_dst, from = c->tx_track[i].relayed_src;
        bool ok = status == MAC_STATUS_SUCCESS;
        if (!ok && status != MAC_STATUS_NO_ACK) {
            /* Channel access failure (busy channel), radio error, purge: says
             * nothing about the link. Treating congestion as broken routes
             * would drop neighbours and add MTORR broadcasts to a saturated
             * channel. Reported to the host, routes kept. */
            c->stats.tx_not_link_failures++;
            return;
        }
        bool dropped = nwk_neighbor_tx_result(&c->routes, hop, ok);
        if (ok) return;
        c->stats.route_failures++;
        if (dropped) {
            c->stats.neighbors_dropped++;
            nwk_routes_forget(&c->routes, hop);   /* every route through it */
        }
        if (hop != dst) {
            /* A relay that did not acknowledge 4 attempts: every path through
             * it is broken, not only this destination's. */
            nwk_routes_forget(&c->routes, dst);
            nwk_routes_forget(&c->routes, hop);
        }
        if (from != 0xFFFF && from != 0x0000) {
            /* We relayed it: tell the originator its route failed. */
            send_network_status(c, from, 0x01 /* tree/link failure */, dst);
            c->stats.relay_failures_reported++;
        }
        if (hop != dst || dropped) {
            early_mtorr(c, ZB_EARLY_MTORR_MS);   /* rebuild the many-to-one routes and route records */
            if (dst != 0x0000 && !nwk_is_broadcast(dst)) {
                disc_heard(c, dst);
                if (!disc_pending_for(c, dst)) (void)zb_coord_discover_route(c, dst);
            }
        }
        return;
    }
}

static void on_data_confirm(void *ctx, uint8_t handle, uint8_t status) {
    zb_coord_t *c = ctx;
    route_outcome(c, handle, status);
    if (c->upper.send_confirm) c->upper.send_confirm(c->upper.ctx, handle, status);
}

static void on_comm_status(void *ctx, uint64_t device, uint8_t status) {
    zb_coord_t *c = ctx;
    bool awaited = false;
    for (unsigned i = 0; i < ZB_AWAITING_KEY; i++) {
        if (device && c->awaiting_key[i] == device) { c->awaiting_key[i] = 0; awaited = true; }
    }
    if (!awaited || status != MAC_STATUS_SUCCESS) return;
    uint16_t slot = nwk_devices_find_ext(&c->devices, device);
    if (slot != NWK_DEVICE_NONE) send_transport_key(c, slot);
}

/* ---- Trust Center: unique link key ------------------------------------ */
static void handle_request_key(zb_coord_t *c, uint16_t src, uint64_t ext, const uint8_t *p, size_t len) {
    c->stats.key_requests++;
    if (len < 2 || p[1] != APS_KEY_TYPE_TC_LINK || (!c->tclk_seed_valid && !c->upper.random_key)) return;
    const uint8_t *current = link_key_for(c, ext);
    if (!current) return;
    int slot = -1;
    for (unsigned i = 0; i < ZB_PENDING_KEYS; i++) {
        if (c->pending_keys[i].used && c->pending_keys[i].ext == ext) { slot = (int)i; break; }
        if (!c->pending_keys[i].used && slot < 0) slot = (int)i;
    }
    if (slot < 0) return;
    if (c->tclk_seed_valid) {
        /* Z-Stack 3.x scheme: the key is derived, the device keeps the shift. */
        uint8_t shift = 0;
        if (c->upper.random_key) {
            uint8_t rnd[16];
            if (c->upper.random_key(c->upper.ctx, rnd)) shift = (uint8_t)(rnd[0] & 0x0Fu);
            wipe(rnd, sizeof rnd);
        }
        zb_tclk_derive(c->tclk_seed, shift, ext, c->pending_keys[slot].key);
        c->pending_shift[slot] = shift;
    } else if (!c->upper.random_key(c->upper.ctx, c->pending_keys[slot].key)) {
        return;
    }
    c->pending_keys[slot].used = true;
    c->pending_keys[slot].ext = ext;
    /* Transport Key, key type TC link key: key, destination, source. */
    uint8_t tk[2 + 16 + 8 + 8];
    tk[0] = APS_CMD_TRANSPORT_KEY;
    tk[1] = APS_KEY_TYPE_TC_LINK;
    for (unsigned i = 0; i < 16; i++) tk[2 + i] = c->pending_keys[slot].key[i];
    put64(tk + 18, ext);
    put64(tk + 26, c->ext_addr);
    uint8_t ktk[16];
    zb_key_transport_key(current, ktk);
    if (send_aps_command(c, src, ktk, APS_KEY_TRANSPORT, tk, sizeof tk) == ZB_SEND_OK) c->stats.link_keys_sent++;
    wipe(ktk, sizeof ktk);
    wipe(tk, sizeof tk);
}

static void handle_verify_key(zb_coord_t *c, uint16_t src, uint64_t ext, const uint8_t *p, size_t len) {
    if (len < 26 || p[1] != APS_KEY_TYPE_TC_LINK || get64(p + 2) != ext) { c->stats.key_verify_failures++; return; }
    int slot = -1;
    for (unsigned i = 0; i < ZB_PENDING_KEYS; i++)
        if (c->pending_keys[i].used && c->pending_keys[i].ext == ext) slot = (int)i;
    uint16_t dev = nwk_devices_find_ext(&c->devices, ext);
    if (slot < 0 || dev == NWK_DEVICE_NONE) { c->stats.key_verify_failures++; return; }
    uint8_t expected[16];
    const uint8_t three = 0x03;
    (void)zb_hmac_mmo(c->pending_keys[slot].key, &three, 1, expected);
    uint8_t diff = 0;
    for (unsigned i = 0; i < 16; i++) diff |= (uint8_t)(expected[i] ^ p[10 + i]);
    wipe(expected, sizeof expected);
    if (diff) { c->stats.key_verify_failures++; return; }
    nwk_device_t *d = &c->devices.dev[dev];
    for (unsigned i = 0; i < 16; i++) d->link_key[i] = c->pending_keys[slot].key[i];
    d->unique_key = true;
    d->aps_in_seen = false;
    d->tclk_shift = c->tclk_seed_valid ? c->pending_shift[slot] : 0xFF;
    wipe(&c->pending_keys[slot], sizeof c->pending_keys[slot]);
    c->stats.keys_verified++;
    save_device(c, dev);
    uint8_t confirm[1 + 1 + 1 + 8] = {APS_CMD_CONFIRM_KEY, 0x00, APS_KEY_TYPE_TC_LINK};
    put64(confirm + 3, ext);
    (void)send_aps_command(c, src, d->link_key, APS_KEY_DATA, confirm, sizeof confirm);
    if (c->upper.key_established) c->upper.key_established(c->upper.ctx, d->short_addr, ext);
}

/* ---- ZDO server --------------------------------------------------------- */
size_t zb_coord_zdo_local(zb_coord_t *c, uint16_t cluster, const uint8_t *p, size_t len, bool broadcast,
                          uint8_t *r, size_t cap) {
    if (len < 1 || cap < 96) return 0;
    size_t n = 0;
    r[n++] = p[0];   /* transaction sequence */
    switch (cluster) {
    case ZDO_NWK_ADDR_REQ:
        if (len < 11 || get64(p + 1) != c->ext_addr) return 0;   /* not us: no answer */
        r[n++] = ZDO_SUCCESS; put64(r + n, c->ext_addr); n += 8; put16(r + n, 0x0000); n += 2;
        r[n++] = 0; r[n++] = 0;   /* no associated device list returned */
        break;
    case ZDO_IEEE_ADDR_REQ:
        if (len < 5 || get16(p + 1) != 0x0000) return 0;
        r[n++] = ZDO_SUCCESS; put64(r + n, c->ext_addr); n += 8; put16(r + n, 0x0000); n += 2;
        r[n++] = 0; r[n++] = 0;
        break;
    case ZDO_NODE_DESC_REQ: {
        if (len < 3) return 0;
        if (get16(p + 1) != 0x0000) { r[n++] = ZDO_DEVICE_NOT_FOUND; put16(r + n, get16(p + 1)); n += 2; break; }
        static const uint8_t node_desc[13] = {
            0x00,             /* coordinator, no complex/user descriptor */
            0x40,             /* 2.4 GHz band */
            0x8F,             /* MAC capability: alt PAN coord, FFD, mains, RX on, alloc */
            0x00, 0x00,       /* manufacturer code (none claimed) */
            0x50,             /* max buffer size 80 */
            0xA0, 0x00,       /* max incoming transfer 160 */
            0x41, 0x2C,       /* server mask: primary TC, network manager, rev 22 */
            0xA0, 0x00,       /* max outgoing transfer 160 */
            0x00,
        };
        r[n++] = ZDO_SUCCESS; put16(r + n, 0x0000); n += 2;
        for (unsigned i = 0; i < sizeof node_desc; i++) r[n++] = node_desc[i];
        break;
    }
    case ZDO_ACTIVE_EP_REQ:
        if (len < 3) return 0;
        r[n++] = get16(p + 1) == 0x0000 ? ZDO_SUCCESS : ZDO_DEVICE_NOT_FOUND;
        put16(r + n, get16(p + 1)); n += 2;
        if (r[1] == ZDO_SUCCESS) {
            r[n++] = c->endpoint_count;
            for (unsigned i = 0; i < c->endpoint_count; i++) r[n++] = c->endpoints[i].endpoint;
        } else {
            r[n++] = 0;
        }
        break;
    case ZDO_SIMPLE_DESC_REQ: {
        if (len < 4) return 0;
        const zb_endpoint_t *ep = get16(p + 1) == 0x0000 ? find_endpoint(c, p[3]) : 0;
        if (!ep) {
            r[n++] = get16(p + 1) != 0x0000 ? ZDO_DEVICE_NOT_FOUND : ZDO_INVALID_EP;
            put16(r + n, get16(p + 1)); n += 2; r[n++] = 0;
            break;
        }
        if (n + 13u + 2u * (ep->in_count + ep->out_count) > cap) return 0;
        r[n++] = ZDO_SUCCESS; put16(r + n, 0x0000); n += 2;
        r[n++] = (uint8_t)(8 + 2 * (ep->in_count + ep->out_count));
        r[n++] = ep->endpoint;
        put16(r + n, ep->profile); n += 2;
        put16(r + n, ep->device_id); n += 2;
        r[n++] = ep->version;
        r[n++] = ep->in_count;
        for (unsigned i = 0; i < ep->in_count; i++) { put16(r + n, ep->in[i]); n += 2; }
        r[n++] = ep->out_count;
        for (unsigned i = 0; i < ep->out_count; i++) { put16(r + n, ep->out[i]); n += 2; }
        break;
    }
    case ZDO_MATCH_DESC_REQ: {
        if (len < 6) return 0;
        uint16_t profile = get16(p + 3);
        size_t pos = 5;
        uint8_t in_n = p[pos++];
        if (len < pos + 2u * in_n + 1u) return 0;
        size_t in_pos = pos;
        pos += 2u * in_n;
        uint8_t out_n = p[pos++];
        if (len < pos + 2u * out_n) return 0;
        size_t out_pos = pos;
        uint8_t matches[ZB_MAX_ENDPOINTS], m = 0;
        for (unsigned e = 0; e < c->endpoint_count; e++) {
            const zb_endpoint_t *ep = &c->endpoints[e];
            if (profile != ep->profile && profile != 0xFFFF) continue;
            bool hit = false;
            for (unsigned i = 0; i < in_n; i++)
                for (unsigned k = 0; k < ep->in_count; k++) hit |= get16(p + in_pos + 2u * i) == ep->in[k];
            for (unsigned i = 0; i < out_n; i++)
                for (unsigned k = 0; k < ep->out_count; k++) hit |= get16(p + out_pos + 2u * i) == ep->out[k];
            if (hit) matches[m++] = ep->endpoint;
        }
        if (!m && broadcast) return 0;   /* broadcasts are only answered on a match */
        r[n++] = ZDO_SUCCESS; put16(r + n, 0x0000); n += 2;
        r[n++] = m;
        for (unsigned i = 0; i < m; i++) r[n++] = matches[i];
        break;
    }
    case 0x0031: {   /* Mgmt_Lqi_req(start index): neighbour routers and children */
        if (len < 2 || broadcast) return 0;
        uint8_t start = p[1];
        uint16_t list[NWK_MAX_DEVICES];
        unsigned total = 0;
        for (uint16_t s = 0; s < NWK_MAX_DEVICES; s++) {
            const nwk_device_t *d = &c->devices.dev[s];
            if (!d->ext) continue;
            if (d->relationship == NWK_REL_CHILD || nwk_neighbor(&c->routes, d->short_addr)) list[total++] = s;
        }
        r[n++] = ZDO_SUCCESS;
        r[n++] = (uint8_t)(total > 255 ? 255 : total);
        r[n++] = start;
        size_t count_at = n++;
        uint8_t count = 0;
        for (unsigned i = start; i < total && count < 3; i++, count++) {
            const nwk_device_t *d = &c->devices.dev[list[i]];
            const nwk_neighbor_t *nb = nwk_neighbor(&c->routes, d->short_addr);
            for (unsigned k = 0; k < 8; k++) r[n++] = c->mac.beacon.ext_pan_id[k];
            put64(r + n, d->ext); n += 8;
            put16(r + n, d->short_addr); n += 2;
            uint8_t type = (d->capability & 0x02u) ? 1 : 2;               /* router / end device */
            uint8_t rx_on = (d->capability & NWK_CAP_RX_ON_IDLE) ? 1 : 0;
            uint8_t rel = d->relationship == NWK_REL_CHILD ? 1 : 2;        /* child / sibling */
            r[n++] = (uint8_t)(type | (rx_on << 2) | (rel << 4));
            r[n++] = 0x02;                                                 /* permit joining unknown */
            r[n++] = 1;                                                    /* depth */
            r[n++] = nb ? nb->lqi : d->lqi;
        }
        r[count_at] = count;
        break;
    }
    case 0x0032: {   /* Mgmt_Rtg_req(start index): next-hop and source routes */
        if (len < 2 || broadcast) return 0;
        uint8_t start = p[1];
        uint16_t dsts[NWK_TABLE_ROUTES + NWK_SOURCE_ROUTES], hops[NWK_TABLE_ROUTES + NWK_SOURCE_ROUTES];
        uint8_t flags[NWK_TABLE_ROUTES + NWK_SOURCE_ROUTES];
        unsigned total = 0;
        for (unsigned i = 0; i < NWK_TABLE_ROUTES; i++) {
            if (!c->routes.table[i].used) continue;
            dsts[total] = c->routes.table[i].dst; hops[total] = c->routes.table[i].next_hop; flags[total++] = 0x00;
        }
        for (unsigned i = 0; i < NWK_SOURCE_ROUTES; i++) {
            const nwk_source_route_t *sr = &c->routes.source[i];
            if (!sr->used) continue;
            dsts[total] = sr->dst;
            hops[total] = sr->count ? sr->relays[sr->count - 1] : sr->dst;
            flags[total++] = 0x20;   /* active, route record (source route) */
        }
        r[n++] = ZDO_SUCCESS;
        r[n++] = (uint8_t)total;
        r[n++] = start;
        size_t count_at = n++;
        uint8_t count = 0;
        for (unsigned i = start; i < total && count < 10; i++, count++) {
            put16(r + n, dsts[i]); n += 2;
            r[n++] = flags[i];
            put16(r + n, hops[i]); n += 2;
        }
        r[count_at] = count;
        break;
    }
    default:
        if (broadcast) return 0;
        r[n++] = ZDO_NOT_SUPPORTED;
        break;
    }
    return n;
}

static void handle_zdo_request(zb_coord_t *c, const nwk_header_t *h, uint16_t cluster, const uint8_t *p, size_t len) {
    c->stats.zdo_requests++;
    uint8_t r[128];
    size_t n = zb_coord_zdo_local(c, cluster, p, len, nwk_is_broadcast(h->dst), r, sizeof r);
    if (!n) return;
    if (zb_coord_send_aps(c, h->src, 0, 0, (uint16_t)(0x8000u | cluster), ZDO_PROFILE, r, (uint8_t)n, false) == ZB_SEND_OK)
        c->stats.zdo_responses_sent++;
}

static void handle_announce(zb_coord_t *c, const nwk_header_t *h, uint16_t mac_src, uint64_t auth_source,
                            const uint8_t *p, size_t len) {
    if (len < 12) { c->stats.aps_malformed++; return; }
    uint16_t short_addr = get16(p + 1);
    uint64_t ext = get64(p + 3);
    c->stats.announces++;
    /* Anti-spoofing: a one-hop announce must come from the announced IEEE
     * (NWK security source); a relayed one carries the relay's IEEE there,
     * so its NWK source must then be the announced address. The device must
     * be known to this Trust Center either way. */
    /* One hop (MAC source = NWK source): the security source must be the
     * announced IEEE. Relayed: the NWK source must be the announced address. */
    bool relayed = mac_src != h->src;
    if (!relayed && ext != auth_source) { c->stats.nwk_unknown_source++; return; }
    if (relayed && h->src != short_addr) { c->stats.nwk_unknown_source++; return; }
    uint16_t slot = nwk_devices_find_ext(&c->devices, ext);
    if (slot == NWK_DEVICE_NONE) { c->stats.nwk_unknown_source++; return; }
    bool first = !c->devices.dev[slot].authorized;
    uint16_t old_short = c->devices.dev[slot].short_addr;
    uint16_t owner = nwk_devices_find_short(&c->devices, short_addr);
    if (owner != NWK_DEVICE_NONE && owner != slot) { address_conflict(c, short_addr, ext); return; }
    if (!nwk_devices_learn(&c->devices, ext, short_addr, NWK_REL_NONE)) return;
    if (old_short != short_addr) {
        nwk_routes_forget(&c->routes, old_short);
        nwk_neighbor_remove(&c->routes, old_short);   /* it will link-status under its new address */
    }
    nwk_device_t *d = &c->devices.dev[slot];
    d->capability = p[11];
    d->capability_guessed = false;
    d->last_seen_ms = c->now_ms;
    if (relayed && d->relationship == NWK_REL_CHILD) d->relationship = NWK_REL_NONE;   /* moved behind a router */
    if (first) c->stats.joined++;
    save_device(c, slot);
    if (c->upper.device_joined) c->upper.device_joined(c->upper.ctx, short_addr, ext, p[11]);
}

/* Network key for a device that joined through router `parent`: the
 * Transport Key (APS-secured with the device's key-transport key) is
 * tunnelled to the parent, which delivers it (Zigbee 4.4.3, APS Tunnel). */
static bool tunnel_transport_key(zb_coord_t *c, uint16_t parent, uint16_t slot) {
    const nwk_device_t *d = &c->devices.dev[slot];
    const uint8_t *tclk = link_key_for(c, d->ext);
    uint32_t fc;
    if (!tclk || !next_aps_counter(c, &fc)) return false;
    tc_transport_key_t p = {
        .tc_link_key = tclk, .network_key = c->network_key, .key_seq = c->key_seq,
        .dst_short = d->short_addr, .dst_ext = d->ext, .tc_ext = c->ext_addr,
        .nwk_seq = 0, .radius = ZB_NWK_RADIUS, .aps_counter = c->aps_counter++, .aps_frame_counter = fc,
    };
    uint8_t frame[MAC_MAX_PAYLOAD];
    size_t n = tc_build_transport_network_key(&p, frame, sizeof frame);
    nwk_header_t inner;
    if (!n || nwk_parse(frame, n, &inner) != NWK_OK) { wipe(frame, sizeof frame); return false; }
    /* Tunnel: APS command (NWK security only) = id, destination IEEE, tunnelled APS frame. */
    uint8_t aps[MAC_MAX_PAYLOAD];
    size_t k = 0;
    aps[k++] = 0x01;                 /* APS command, unicast */
    aps[k++] = c->aps_counter++;
    aps[k++] = APS_CMD_TUNNEL;
    put64(aps + k, d->ext); k += 8;
    size_t inner_len = n - inner.header_len;
    if (k + inner_len > sizeof aps) { wipe(frame, sizeof frame); return false; }
    for (size_t i = 0; i < inner_len; i++) aps[k + i] = frame[inner.header_len + i];
    k += inner_len;
    wipe(frame, sizeof frame);
    bool ok = send_nwk(c, parent, aps, k) == ZB_SEND_OK;
    wipe(aps, sizeof aps);
    if (ok) c->stats.tunnels_sent++;
    return ok;
}

static void device_gone(zb_coord_t *c, uint16_t short_addr, uint64_t ext, bool rejoin) {
    c->stats.leaves++;
    if (c->upper.device_left) c->upper.device_left(c->upper.ctx, short_addr, ext, rejoin);
    if (!rejoin && ext) (void)zb_coord_remove_device(c, ext);
    nwk_routes_forget(&c->routes, short_addr);
    nwk_neighbor_remove(&c->routes, short_addr);
}

/* Update Device from a router (Zigbee 4.4.10.2): status 0 secured rejoin,
 * 1 unsecured join, 2 device left, 3 Trust Center rejoin. */
static void handle_update_device(zb_coord_t *c, uint16_t parent, const uint8_t *p, size_t len) {
    if (len < 12) return;
    uint64_t ext = get64(p + 1);
    uint16_t short_addr = get16(p + 9);
    uint8_t status = p[11];
    c->stats.update_device++;
    if (!ext || short_addr >= 0xFFF8u || short_addr == 0x0000) return;
    if (status == 2) { device_gone(c, short_addr, ext, false); return; }
    uint16_t slot = nwk_devices_find_ext(&c->devices, ext);
    bool known = slot != NWK_DEVICE_NONE;
    if (status == 1 && !c->permit) return;   /* joining closed on the Trust Center */
    if (status == 3 && !c->permit && !(known && c->devices.dev[slot].unique_key)) return;
    if (status != 0 || known) {
        uint16_t owner = nwk_devices_find_short(&c->devices, short_addr);
        if (owner != NWK_DEVICE_NONE && owner != slot) { address_conflict(c, short_addr, ext); return; }
        if (!nwk_devices_learn(&c->devices, ext, short_addr, NWK_REL_NONE)) return;
        slot = nwk_devices_find_ext(&c->devices, ext);
    }
    if (slot == NWK_DEVICE_NONE) return;   /* secured rejoin of an unknown device */
    nwk_device_t *d = &c->devices.dev[slot];
    nwk_routes_forget(&c->routes, short_addr);
    const uint16_t relay = parent;
    nwk_source_route_set(&c->routes, short_addr, &relay, 1);
    if (d->relationship == NWK_REL_CHILD) d->relationship = NWK_REL_NONE;
    if (status == 1) {
        /* Fresh join: any previous unique key is gone with the device's reset. */
        d->unique_key = false;
        d->aps_in_seen = false;
        d->authorized = false;
    }
    save_device(c, slot);
    if (status == 1 || status == 3) {
        if (!tunnel_transport_key(c, parent, slot)) c->stats.transport_key_failures++;
        else c->stats.transport_keys_sent++;
    }
}

static void send_aps_ack(zb_coord_t *c, const nwk_header_t *h, const aps_header_t *a, const uint8_t *key) {
    aps_header_t ack = {0};
    ack.type = APS_TYPE_ACK;
    ack.delivery = APS_DELIVERY_UNICAST;
    ack.dst_endpoint = a->src_endpoint;
    ack.src_endpoint = a->dst_endpoint;
    ack.cluster = a->cluster;
    ack.profile = a->profile;
    ack.counter = a->counter;
    if (a->extended && a->fragmentation != APS_FRAG_NONE) {
        /* Window of one block: acknowledge this block (first one = 0). */
        ack.extended = true;
        ack.fragmentation = a->fragmentation;
        ack.block_number = a->fragmentation == APS_FRAG_FIRST ? 0 : a->block_number;
        ack.ack_bitfield = 0x01;
    }
    uint8_t hdr[16];
    size_t hl = aps_build(&ack, hdr, sizeof hdr);
    if (!hl) return;
    uint8_t status;
    c->sending_ack = true;
    if (key) {
        uint32_t fc;
        if (!next_aps_counter(c, &fc)) { c->sending_ack = false; return; }
        nwk_aux_t aux = {.key_id = APS_KEY_DATA, .extended_nonce = true, .frame_counter = fc,
                         .source = c->ext_addr};
        uint8_t aps[MAC_MAX_PAYLOAD];
        size_t n = aps_seal(key, hdr, hl, &aux, 0, 0, aps, sizeof aps);
        status = n ? send_nwk(c, h->src, aps, n) : ZB_SEND_SECURITY;
    } else {
        status = send_nwk(c, h->src, hdr, hl);
    }
    c->sending_ack = false;
    if (status == ZB_SEND_OK) c->stats.aps_acks_sent++;
}

/* ---- NWK commands ----------------------------------------------------- */
static void send_rejoin_response(zb_coord_t *c, uint16_t slot, uint8_t status, bool secured) {
    const nwk_device_t *d = &c->devices.dev[slot];
    const uint8_t p[4] = {NWK_CMD_REJOIN_RESPONSE, (uint8_t)d->short_addr, (uint8_t)(d->short_addr >> 8), status};
    (void)nwk_tx(c, NWK_TYPE_COMMAND, d->short_addr, 1, p, sizeof p, d->ext, secured,
                 !(d->capability & NWK_CAP_RX_ON_IDLE));
}

/* Rejoin Request (one hop, MAC source = the device's IEEE). */
static void handle_rejoin(zb_coord_t *c, const mac_frame_t *f, const nwk_header_t *h, uint8_t capability, bool secured) {
    if (f->src_mode != MAC_ADDR_EXT) return;
    uint64_t ext = f->src_ext;
    uint16_t slot = nwk_devices_find_ext(&c->devices, ext);
    if (slot == NWK_DEVICE_NONE) return;   /* unknown to the Trust Center */
    nwk_device_t *d = &c->devices.dev[slot];
    if (!secured) {
        /* Trust Center rejoin: only with a key the device can really hold. */
        if (!d->unique_key && !c->permit) return;
        if (!link_key_for(c, ext)) return;
    }
    uint16_t want = h->src < 0xFFF8u && h->src != 0x0000 ? h->src : d->short_addr;
    uint16_t owner = nwk_devices_find_short(&c->devices, want);
    if (owner != NWK_DEVICE_NONE && owner != slot) want = d->short_addr;
    if (!nwk_devices_learn(&c->devices, ext, want, NWK_REL_CHILD)) return;
    nwk_routes_forget(&c->routes, want);
    d->capability = capability;
    d->capability_guessed = false;
    d->last_seen_ms = c->now_ms;
    save_device(c, slot);
    send_rejoin_response(c, slot, 0x00, secured);
    if (secured) c->stats.rejoins++;
    else { c->stats.tc_rejoins++; send_transport_key(c, slot); }
}

static void handle_nwk_command(zb_coord_t *c, const mac_frame_t *f, const nwk_header_t *h, const uint8_t *p,
                               size_t len, uint8_t lqi) {
    if (!len) return;
    c->stats.nwk_commands++;
    uint16_t mac_src = f->src_mode == MAC_ADDR_SHORT ? f->src_short : 0xFFFF;
    switch (p[0]) {
    case NWK_CMD_ROUTE_REQUEST: {
        if (len < 6) return;
        c->stats.rreq_rx++;
        uint8_t options = p[1], id = p[2], cost = p[5];
        uint16_t target = get16(p + 3);
        if (options & 0x18u) return;                 /* another concentrator's many-to-one request */
        if (mac_src >= 0xFFF8u || h->src >= 0xFFF8u || h->src == 0x0000) return;
        uint8_t total = (uint8_t)(cost + nwk_lqi_cost(lqi));
        /* Route discovery table: keep the best (lowest cost) reverse path. */
        unsigned d = ZB_ROUTE_DISCOVERIES, free_d = ZB_ROUTE_DISCOVERIES, oldest = 0;
        for (unsigned i = 0; i < ZB_ROUTE_DISCOVERIES; i++) {
            if (c->rdisc[i].used && c->now_ms - c->rdisc[i].ms > 10000u) c->rdisc[i].used = false;
            if (c->rdisc[i].used && c->rdisc[i].originator == h->src && c->rdisc[i].id == id) d = i;
            if (!c->rdisc[i].used && free_d == ZB_ROUTE_DISCOVERIES) free_d = i;
            if (c->rdisc[i].ms < c->rdisc[oldest].ms) oldest = i;
        }
        if (d < ZB_ROUTE_DISCOVERIES && total >= c->rdisc[d].cost) return;   /* not a better path */
        if (d == ZB_ROUTE_DISCOVERIES) d = free_d < ZB_ROUTE_DISCOVERIES ? free_d : oldest;
        c->rdisc[d].used = true;
        c->rdisc[d].originator = h->src;
        c->rdisc[d].id = id;
        c->rdisc[d].cost = total;
        c->rdisc[d].sender = mac_src;
        c->rdisc[d].ms = c->now_ms;
        nwk_table_route_set(&c->routes, h->src, mac_src);   /* reverse route */
        uint16_t slot = nwk_devices_find_short(&c->devices, target);
        bool child_ed = slot != NWK_DEVICE_NONE && c->devices.dev[slot].relationship == NWK_REL_CHILD &&
                        !(c->devices.dev[slot].capability & 0x02u);
        if (target == 0x0000 || child_ed) {
            /* We answer (for ourselves or our end-device child); the reply
             * travels hop by hop, addressed to the previous hop. */
            const uint8_t rrep[8] = {NWK_CMD_ROUTE_REPLY, 0x00, id, (uint8_t)h->src, (uint8_t)(h->src >> 8),
                                     (uint8_t)target, (uint8_t)(target >> 8), total};
            (void)send_nwk_command(c, mac_src, 1, rrep, sizeof rrep);
            return;
        }
        if (h->radius <= 1) return;
        /* Relay the request with the accumulated cost (jittered). */
        nwk_header_t rh = *h;
        rh.security = false;
        rh.radius--;
        uint8_t plain[MAC_MAX_PAYLOAD];
        size_t phl = nwk_build(&rh, plain, sizeof plain);
        if (!phl || phl + len > sizeof plain) return;
        for (size_t i = 0; i < len; i++) plain[phl + i] = p[i];
        plain[phl + 5] = total;
        bcast_queue(c, plain, phl, phl + len, h->src, h->seq, c->now_ms + jitter_ms(c), 1, false, 0);
        c->stats.rreq_relayed++;
        return;
    }
    case NWK_CMD_ROUTE_REPLY: {
        if (len < 8) return;
        c->stats.rrep_rx++;
        uint16_t originator = get16(p + 3), responder = get16(p + 5);
        if (mac_src >= 0xFFF8u) return;
        nwk_table_route_set(&c->routes, responder, mac_src);   /* forward route */
        if (originator == 0x0000) return;
        /* Relayed discovery: pass the reply to the previous hop. */
        for (unsigned i = 0; i < ZB_ROUTE_DISCOVERIES; i++) {
            if (!c->rdisc[i].used || c->rdisc[i].originator != originator || c->rdisc[i].id != p[2]) continue;
            uint8_t rrep[8];
            for (unsigned k = 0; k < 8; k++) rrep[k] = p[k];
            rrep[7] = (uint8_t)(p[7] + nwk_lqi_cost(lqi));
            if (send_nwk_command(c, c->rdisc[i].sender, 1, rrep, sizeof rrep) == ZB_SEND_OK) c->stats.rrep_relayed++;
            return;
        }
        return;
    }
    case NWK_CMD_NETWORK_STATUS: {
        if (len < 4) return;
        c->stats.network_status_rx++;
        uint8_t code = p[1];
        uint16_t about = get16(p + 2);
        if (code <= 0x02u || code == 0x0Bu || code == 0x0Du) nwk_routes_forget(&c->routes, about);
        /* Route failures (incl. many-to-one 0x0C): rebuild the routes now. */
        if (code <= 0x02u || code == 0x0Bu || code == 0x0Cu) {
            bool retransmitted = false;
            if (about != 0x0000 && !nwk_is_broadcast(about)) {
                disc_heard(c, about);   /* clear failure backoff so route discovery can proceed */
                bool already_pending = disc_pending_for(c, about);
                for (unsigned idx = 0; idx < ZB_RECENT_TX; idx++) {
                    if (c->recent_tx[idx].used && !c->recent_tx[idx].answered && c->recent_tx[idx].dst == about &&
                        c->recent_tx[idx].retries < 2 &&
                        (int32_t)(c->now_ms - c->recent_tx[idx].sent_ms) < 4000) {
                        c->recent_tx[idx].retries++;
                        c->recent_tx[idx].sent_ms = c->now_ms;
                        if (c->recent_tx[idx].expects_reply) c->recent_tx[idx].payload[7] = c->aps_counter++;
                        if (!already_pending) (void)zb_coord_discover_route(c, about);
                        (void)disc_hold(c, about, c->recent_tx[idx].radius, c->recent_tx[idx].payload,
                                        c->recent_tx[idx].len, c->recent_tx[idx].secured, false);
                        c->stats.net_status_retries++;
                        retransmitted = true;
                        break;
                    }
                }
                if (!retransmitted && !already_pending) {
                    (void)zb_coord_discover_route(c, about);
                }
            }
            early_mtorr(c, MTORR_MIN_GAP_MS);
        }
        return;
    }
    case NWK_CMD_LEAVE: {
        if (len < 2 || (p[1] & 0x40u)) return;       /* a request to leave is not for the coordinator */
        uint64_t ext = h->has_src_ieee ? h->src_ieee : 0;
        uint16_t slot = ext ? nwk_devices_find_ext(&c->devices, ext) : nwk_devices_find_short(&c->devices, h->src);
        if (slot != NWK_DEVICE_NONE) ext = c->devices.dev[slot].ext;
        device_gone(c, h->src, ext, (p[1] & 0x20u) != 0);
        return;
    }
    case NWK_CMD_ROUTE_RECORD: {
        if (len < 2 || p[1] > NWK_ROUTE_MAX_RELAYS || len < 2u + 2u * p[1]) return;
        c->stats.route_records++;
        uint16_t relays[NWK_ROUTE_MAX_RELAYS];
        for (unsigned i = 0; i < p[1]; i++) relays[i] = get16(p + 2 + 2 * i);
        nwk_source_route_set(&c->routes, h->src, relays, p[1]);
        return;
    }
    case NWK_CMD_REJOIN_REQUEST:
        if (len >= 2) handle_rejoin(c, f, h, p[1], true);
        return;
    case NWK_CMD_LINK_STATUS: {
        if (len < 2 || h->radius > 1 || mac_src != h->src) return;
        size_t count = p[1] & 0x1Fu;
        if (len < 2 + 3 * count) return;
        c->stats.link_status_rx++;
        if (nwk_neighbor_link_status(&c->routes, h->src, lqi, 0x0000, p + 2, count, (p[1] & 0x60u) == 0x60u)) {
            /* A neighbour restarted: its many-to-one route (and those of the
             * routers behind it) is gone. Rebuild now rather than at the next
             * periodic request (up to 2 minutes of silent reports). */
            c->stats.neighbor_restarts++;
            early_mtorr(c, ZB_EARLY_MTORR_MS);
        }
        return;
    }
    case NWK_CMD_ED_TIMEOUT_REQUEST: {
        uint16_t slot = nwk_devices_find_short(&c->devices, h->src);
        if (slot == NWK_DEVICE_NONE || c->devices.dev[slot].relationship != NWK_REL_CHILD) return;
        c->stats.timeout_requests++;
        const uint8_t rsp[3] = {NWK_CMD_ED_TIMEOUT_RESPONSE, 0x00 /* success */, 0x01 /* MAC poll keep-alive */};
        (void)send_nwk_command(c, h->src, 1, rsp, sizeof rsp);
        return;
    }
    default:
        return;
    }
}

/* Relay a decrypted unicast that is not for us (one more hop towards a
 * child, a neighbour or a known route), re-secured with our counter. */
static void relay(zb_coord_t *c, const nwk_header_t *in, const uint8_t *payload, size_t len) {
    hop_t hop = {.mac_dst = in->dst};
    nwk_header_t h = *in;
    if (in->radius <= 1) { c->stats.relay_dropped++; return; }
    if (in->source_route) {
        /* We must be the relay the index points at (Zigbee PRO 3.6.3.3.2). */
        if (in->relays[in->relay_index] != 0x0000) { c->stats.relay_dropped++; return; }
        if (in->relay_index == 0) {
            hop_t direct;
            if (next_hop(c, in->dst, &direct) && !direct.source) hop = direct;   /* child: maybe indirect */
        } else {
            h.relay_index--;
            hop.mac_dst = in->relays[h.relay_index];
        }
        c->stats.source_relayed++;
    } else if (!next_hop(c, in->dst, &hop) || hop.source) {
        c->stats.relay_dropped++;
        if (in->type == NWK_TYPE_DATA && in->src != 0x0000) send_network_status(c, in->src, 0x00, in->dst);
        return;
    }
    h.security = false;
    h.radius--;
    uint8_t frame[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(&h, frame, sizeof frame);
    if (!hl || hl + len + 18u > MAC_MAX_PAYLOAD) { c->stats.relay_dropped++; return; }
    for (size_t i = 0; i < len; i++) frame[hl + i] = payload[i];
    uint8_t local[8];
    put64(local, c->ext_addr);
    size_t total = 0;
    if (core_security_seal(&c->security, c->tx_key_slot, local, frame, hl, len, sizeof frame, &total) &&
        mac_send(c, MAC_ADDR_SHORT, hop.mac_dst, 0, hop.indirect, frame, total) == ZB_SEND_OK) {
        c->stats.relayed++;
        track_tx(c, c->last_send_handle, in->dst, hop.mac_dst, in->src);
    } else {
        c->stats.relay_dropped++;
    }
    wipe(frame, sizeof frame);
}

static void on_mac_data_inner(void *ctx, const mac_frame_t *f, uint8_t lqi, int8_t rssi);
static void on_mac_data(void *ctx, const mac_frame_t *f, uint8_t lqi, int8_t rssi) {
    on_mac_data_inner(ctx, f, lqi, rssi);
    disc_service(ctx);   /* a Route Reply / Record / neighbour may have given a route */
}

static void on_mac_data_inner(void *ctx, const mac_frame_t *f, uint8_t lqi, int8_t rssi) {
    zb_coord_t *c = ctx;
    (void)rssi;
    c->stats.nwk_rx++;
    if (f->payload_len >= 1 && ((f->payload[0] >> 2) & 0x0Fu) == 3u) {
        /* Green Power (protocol version 3): handed to the GP proxy as is. */
        c->stats.gpdf_rx++;
        if (c->upper.gpdf) c->upper.gpdf(c->upper.ctx, f->payload, f->payload_len, f->seq, lqi);
        return;
    }
    if (f->payload_len > CORE_SECURITY_MAX_FRAME) { c->stats.nwk_malformed++; return; }
    uint8_t frame[CORE_SECURITY_MAX_FRAME];
    memcpy(frame, f->payload, f->payload_len);
    nwk_header_t h;
    if (nwk_parse(frame, f->payload_len, &h) != NWK_OK) { c->stats.nwk_malformed++; return; }
    /* After joining, every NWK frame must be secured with the network key,
     * except the Trust Center rejoin request of a device that lost it. */
    if (!h.security) {
        if (h.type == NWK_TYPE_COMMAND && h.payload_len >= 2 && h.payload[0] == NWK_CMD_REJOIN_REQUEST &&
            h.dst == 0x0000)
            handle_rejoin(c, f, &h, h.payload[1], false);
        else
            c->stats.nwk_unsecured_dropped++;
        return;
    }
    /* Copy of a broadcast already received (or sent by us): the BTT only
     * holds AUTHENTICATED broadcasts, so this frame is either a relayed copy
     * or a forgery, dropped either way; no CCM* needed (the relayed copies
     * of every broadcast arrive from each neighbour: most of the receive
     * work under load). The passive acknowledgement is credited without
     * authentication: a forged copy could at worst spare one retransmission
     * of a broadcast, which a jammer in radio range can prevent anyway. */
    if (nwk_is_broadcast(h.dst) && nwk_btt_seen(&c->routes, h.src, h.seq, c->now_ms)) {
        c->stats.broadcast_duplicates++;
        c->stats.bcast_dup_unopened++;
        if (f->src_mode == MAC_ADDR_SHORT) bcast_heard(c, h.src, h.seq, f->src_short);
        return;
    }
    size_t payload_len = 0;
    if (!core_security_open_authenticated_peer(&c->security, frame, f->payload_len, h.header_len, &payload_len)) {
        c->stats.nwk_auth_failures++;
        return;
    }
    uint64_t aux_source;
    memcpy(&aux_source, h.payload + 5, sizeof aux_source);
    uint16_t dev = nwk_devices_find_ext(&c->devices, aux_source);
    if (dev == NWK_DEVICE_NONE) {
        /* Authenticated by the network key, hence a network member we did
         * not know (e.g. a device herdsman's restore could not carry): learn
         * the transmitting node (security source = MAC source's IEEE). */
        c->stats.nwk_unknown_source++;
        if (f->src_mode != MAC_ADDR_SHORT || !nwk_devices_learn(&c->devices, aux_source, f->src_short, NWK_REL_NONE))
            return;
        dev = nwk_devices_find_ext(&c->devices, aux_source);
        if (dev == NWK_DEVICE_NONE) return;
        c->devices.dev[dev].authorized = true;
        save_device(c, dev);
        c->stats.learned_members++;
    }
    c->devices.dev[dev].last_seen_ms = c->now_ms;
    disc_heard(c, h.src);
    if (f->src_mode == MAC_ADDR_SHORT) disc_heard(c, f->src_short);
    if (f->src_mode == MAC_ADDR_SHORT) {
        nwk_neighbor_heard(&c->routes, f->src_short, lqi);
        uint16_t hop = nwk_devices_find_short(&c->devices, f->src_short);
        if (hop != NWK_DEVICE_NONE) c->devices.dev[hop].lqi = lqi;
    }
    uint8_t *aps_bytes = frame + h.header_len + 14u;
    bool broadcast = nwk_is_broadcast(h.dst);
    if (broadcast && !nwk_btt_accept(&c->routes, h.src, h.seq, c->now_ms)) {
        c->stats.broadcast_duplicates++;
        if (f->src_mode == MAC_ADDR_SHORT) bcast_heard(c, h.src, h.seq, f->src_short);   /* passive ACK */
        return;
    }
    if (!broadcast && h.dst != 0x0000) {
        c->stats.not_for_us++;
        relay(c, &h, aps_bytes, payload_len);
        return;
    }
    if (broadcast && h.radius > 1 && h.src != 0x0000 &&
        !(h.type == NWK_TYPE_COMMAND && payload_len && (aps_bytes[0] == NWK_CMD_ROUTE_REQUEST ||
                                                        aps_bytes[0] == NWK_CMD_LINK_STATUS))) {
        /* Router duty: relay the broadcast once (jitter), passive-ACK retries. */
        nwk_header_t rh = h;
        rh.security = false;
        rh.radius--;
        uint8_t plain[MAC_MAX_PAYLOAD];
        size_t phl = nwk_build(&rh, plain, sizeof plain);
        if (phl && phl + payload_len <= sizeof plain) {
            for (size_t i = 0; i < payload_len; i++) plain[phl + i] = aps_bytes[i];
            bcast_queue(c, plain, phl, phl + payload_len, h.src, h.seq, c->now_ms + jitter_ms(c),
                        1 + ZB_BCAST_RETRIES, h.dst == NWK_BCAST_ALL, 0);
        }
        wipe(plain, sizeof plain);
    }
    if (h.type == NWK_TYPE_COMMAND) { handle_nwk_command(c, f, &h, aps_bytes, payload_len, lqi); return; }
    if (h.type != NWK_TYPE_DATA) return;
    aps_header_t a;
    if (aps_parse(aps_bytes, payload_len, &a) != APS_OK) { c->stats.aps_malformed++; return; }

    const uint8_t *aps_key = 0;
    if (a.security) {
        nwk_aux_t aa;
        if (nwk_aux_parse(aps_bytes + a.header_len, payload_len - a.header_len, &aa) != NWK_OK ||
            aa.key_id != APS_KEY_DATA || !aa.extended_nonce) { c->stats.aps_auth_failures++; return; }
        uint16_t sdev = nwk_devices_find_ext(&c->devices, aa.source);
        aps_key = sdev == NWK_DEVICE_NONE ? 0 : link_key_for(c, aa.source);
        size_t off = 0, plen = 0;
        if (!aps_key || !aps_open(aps_key, aps_bytes, payload_len, a.header_len, &aa, &off, &plen)) {
            c->stats.aps_auth_failures++;
            return;
        }
        nwk_device_t *sd = &c->devices.dev[sdev];
        if (sd->aps_in_seen && aa.frame_counter <= sd->aps_in_counter) { c->stats.aps_replays++; return; }
        bool persist = !sd->aps_in_seen || (aa.frame_counter >> 10) != (sd->aps_in_counter >> 10);
        sd->aps_in_counter = aa.frame_counter;
        sd->aps_in_seen = true;
        /* Durable every 1024 counter values: after a reboot, replays are
         * refused below the stored value (window bounded by 1024). */
        if (persist) save_device(c, sdev);
        a.payload = aps_bytes + off;
        a.payload_len = plen;
    }

    if (a.type == APS_TYPE_ACK) { c->stats.aps_acks_rx++; frag_ack(c, h.src, &a); return; }
    if (a.type == APS_TYPE_DATA && a.extended && a.fragmentation != APS_FRAG_NONE) {
        /* All blocks share one APS counter: reassembled before the
         * duplicate filter, acknowledged block by block. */
        if (a.delivery == APS_DELIVERY_UNICAST && !nwk_is_broadcast(h.dst))
            frag_rx(c, &h, &a, a.security ? aps_key : 0, lqi);
        return;
    }
    if (a.type == APS_TYPE_COMMAND) {
        if (a.payload_len < 1) return;
        uint64_t sender = aux_source;
        if (a.payload[0] == APS_CMD_REQUEST_KEY && a.security)
            handle_request_key(c, h.src, sender, a.payload, a.payload_len);
        else if (a.payload[0] == APS_CMD_UPDATE_DEVICE)   /* APS-secured (R21) or NWK-only (legacy) */
            handle_update_device(c, h.src, a.payload, a.payload_len);
        else if (a.payload[0] == APS_CMD_VERIFY_KEY && !a.security)
            handle_verify_key(c, h.src, sender, a.payload, a.payload_len);
        return;
    }
    if (a.ack_request && !nwk_is_broadcast(h.dst) && a.delivery == APS_DELIVERY_UNICAST)
        send_aps_ack(c, &h, &a, a.security ? aps_key : 0);   /* re-acknowledged even if duplicate */
    bool answered_twice = false;
    for (unsigned i = 0; i < ZB_RECENT_TX; i++) {
        zb_recent_tx_t *e = &c->recent_tx[i];
        if (!e->used || e->dst != h.src) continue;
        if (!e->expects_reply) { e->used = false; continue; }
        uint16_t req_cluster = (uint16_t)(e->payload[2] | (e->payload[3] << 8));
        if (a.cluster != req_cluster || a.payload_len < 3 || a.payload[1] != e->payload[9]) continue;
        /* A re-sent read can be answered twice (the first answer was only
         * late): the second answer, same command from the server side, is
         * not passed up again. Entries never re-sent are freed at once. */
        if (e->answered) { answered_twice |= (a.payload[0] & 0x08u) && a.payload[2] == e->answer_cmd; continue; }
        note_answer(c, h.src);
        if (e->retries) { e->answered = true; e->answer_cmd = a.payload[2]; }
        else e->used = false;
    }
    if (answered_twice) { c->stats.zcl_answers_dropped++; return; }
    if (!aps_fresh(c, h.src, a.counter, a.cluster, a.payload, a.payload_len)) { c->stats.aps_duplicates++; return; }
    if (a.profile == ZDO_PROFILE && a.dst_endpoint == 0 && a.delivery != APS_DELIVERY_GROUP) {
        if (a.cluster == ZDO_DEVICE_ANNOUNCE) {
            handle_announce(c, &h, f->src_mode == MAC_ADDR_SHORT ? f->src_short : 0xFFFF, aux_source, a.payload,
                            a.payload_len);
            return;
        }
        if (a.cluster < 0x8000u) {
            handle_zdo_request(c, &h, a.cluster, a.payload, a.payload_len);
            return;
        }
    }
    /* Node_Desc_rsp passing up (host interview): its MAC capability tells a
     * sleepy device from a listening one, which a backup does not carry
     * (imported devices stay "guessed" otherwise). Stored only on change. */
    if (a.profile == ZDO_PROFILE && a.cluster == 0x8002u && a.payload_len >= 17 && a.payload[1] == ZDO_SUCCESS) {
        uint16_t s = nwk_devices_find_short(&c->devices, get16(a.payload + 2));
        if (s != NWK_DEVICE_NONE) {
            nwk_device_t *d = &c->devices.dev[s];
            uint8_t cap = a.payload[6];
            if (d->capability_guessed || d->capability != cap) {
                d->capability = cap;
                d->capability_guessed = false;
                c->stats.capabilities_learnt++;
                save_device(c, s);
            }
        }
    }
    if (c->upper.aps_data) {
        c->stats.aps_data_up++;
        c->upper.aps_data(c->upper.ctx, &h, &a, a.payload, a.payload_len, lqi);
    }
}

/* ---- public API ----------------------------------------------------- */
void zb_coord_init(zb_coord_t *c, const mac_radio_ops_t *radio, const zb_coord_upper_t *upper,
                   uint64_t ext_addr, uint32_t random_seed, uint32_t nwk_counter_upper) {
    *c = (zb_coord_t){0};
    if (upper) c->upper = *upper;
    c->ext_addr = ext_addr;
    c->allow_default_tclk = true;
    const mac_upper_t mac_upper = {on_data_confirm, on_mac_data, on_assoc, 0, on_comm_status, on_poll, c};
    mac_init(&c->mac, radio, &mac_upper, ext_addr);
    nwk_devices_init(&c->devices, random_seed);
    core_security_init(&c->security, nwk_counter_upper, zb_ccm_backend_active, reserve_adapter, c);
    c->nwk_seq = (uint8_t)random_seed;
    c->aps_counter = (uint8_t)(random_seed >> 8);
    c->zdo_seq = (uint8_t)(random_seed >> 16);
    c->endpoints[0] = DEFAULT_EP1;
    c->endpoint_count = 1;
    c->forced_handle = -1;
}

void zb_coord_form(zb_coord_t *c, uint16_t pan_id, uint8_t channel, const uint8_t ext_pan_id[8],
                   const uint8_t network_key[16], uint8_t key_seq) {
    for (unsigned i = 0; i < 16; i++) c->network_key[i] = network_key[i];
    c->key_seq = key_seq;
    c->security.network_keys[1].valid = false;
    c->tx_key_slot = 0;
    c->pending_key_valid = c->key_switch_due = false;
    (void)core_security_set_key(&c->security, 0, key_seq, network_key);
    if (!c->tclk_seed_valid && c->upper.random_key && c->upper.random_key(c->upper.ctx, c->tclk_seed))
        c->tclk_seed_valid = true;
    mac_start(&c->mac, pan_id, channel, ext_pan_id);
    /* Counters first: a network record never exists without the counters
     * bound to its key (zb_coord_restore refuses that state). */
    (void)save_counters(c, c->nwk_reserved, c->aps_reserved);
    save_network(c);
}

void zb_coord_set_network(zb_coord_t *c, uint16_t pan_id, uint8_t channel, const uint8_t ext_pan_id[8],
                          uint8_t update_id) {
    c->nwk_update_id = update_id;
    mac_start(&c->mac, pan_id, channel, ext_pan_id);
    save_network(c);
}

void zb_coord_set_network_key(zb_coord_t *c, const uint8_t key[16], uint8_t key_seq) {
    for (unsigned i = 0; i < 16; i++) c->network_key[i] = key[i];
    c->key_seq = key_seq;
    c->security.network_keys[1].valid = false;   /* a restored key replaces any rotation */
    c->tx_key_slot = 0;
    c->pending_key_valid = c->key_switch_due = false;
    (void)core_security_set_key(&c->security, 0, key_seq, key);
    /* Counters keep their (monotonic) values and are re-bound to the key. */
    (void)save_counters(c, c->nwk_reserved, c->aps_reserved);
    save_network(c);
}

void zb_coord_forget(zb_coord_t *c) {
    if (c->store) {
        (void)nv_log_delete(c->store, STORE_ID_NETWORK);
        for (uint16_t slot = 0; slot < NWK_MAX_DEVICES; slot++)
            if (c->devices.dev[slot].ext) (void)nv_log_delete(c->store, (uint16_t)(STORE_ID_DEVICE0 + slot));
    }
    nwk_devices_init(&c->devices, c->devices.rng);
    nwk_routes_clear(&c->routes);
    for (unsigned i = 0; i < ZB_BCAST_PENDING; i++) c->bcast[i].used = false;
    for (unsigned i = 0; i < ZB_ROUTE_DISCOVERIES; i++) c->rdisc[i].used = false;
    wipe(c->network_key, sizeof c->network_key);
    wipe(c->tclk_seed, sizeof c->tclk_seed);
    c->tclk_seed_valid = false;
    zb_coord_permit_join(c, 0);
}

bool zb_coord_remove_device(zb_coord_t *c, uint64_t ext) {
    uint16_t slot = nwk_devices_find_ext(&c->devices, ext);
    if (slot == NWK_DEVICE_NONE) return false;
    (void)nwk_devices_remove(&c->devices, ext);
    if (c->store) (void)nv_log_delete(c->store, (uint16_t)(STORE_ID_DEVICE0 + slot));
    return true;
}

void zb_coord_device_changed(zb_coord_t *c, uint16_t slot) { save_device(c, slot); }

void zb_coord_raise_aps_frame_counter(zb_coord_t *c, uint32_t value) {
    if (value <= c->tc_aps_frame_counter) return;
    /* Durable at once; next_aps_counter() then reserves a window from it. */
    if (c->store && !save_counters(c, c->nwk_reserved, value)) return;
    c->tc_aps_frame_counter = c->aps_reserved = value;
}

void zb_coord_set_tclk_seed(zb_coord_t *c, const uint8_t seed[16]) {
    for (unsigned i = 0; i < 16; i++) c->tclk_seed[i] = seed[i];
    c->tclk_seed_valid = true;
    save_network(c);
}

/* APS command, APS-unsecured, NWK-secured with the current key (broadcast
 * network key update / switch, as Z-Stack). */
static uint8_t send_aps_command_plain(zb_coord_t *c, uint16_t dst, const uint8_t *payload, size_t len) {
    bool broadcast = nwk_is_broadcast(dst);
    uint8_t aps[2 + 1 + 1 + 16 + 1 + 8 + 8];   /* header + Transport Key (the longest command) */
    if (len + 2u > sizeof aps) return ZB_SEND_TOO_LONG;
    aps[0] = (uint8_t)(0x01u | (broadcast ? 0x08u : 0x00u));   /* command frame, broadcast/unicast delivery */
    aps[1] = c->aps_counter++;
    for (size_t i = 0; i < len; i++) aps[2 + i] = payload[i];
    return send_nwk(c, dst, aps, len + 2u);
}

bool zb_coord_update_network_key(zb_coord_t *c, uint16_t dst, uint8_t seq, const uint8_t key[16]) {
    if (!c->mac.started || seq == c->key_seq || c->key_switch_due) return false;
    unsigned alt = 1u - c->tx_key_slot;
    if (!core_security_set_key(&c->security, alt, seq, key)) return false;
    uint8_t tk[1 + 1 + 16 + 1 + 8 + 8] = {APS_CMD_TRANSPORT_KEY, APS_KEY_TYPE_STANDARD_NWK};
    for (unsigned i = 0; i < 16; i++) tk[2 + i] = key[i];
    tk[18] = seq;
    uint64_t dst_ext = 0;   /* broadcast: no single destination */
    if (!nwk_is_broadcast(dst)) {
        uint16_t slot = nwk_devices_find_short(&c->devices, dst);
        if (slot != NWK_DEVICE_NONE) dst_ext = c->devices.dev[slot].ext;
    }
    put64(tk + 19, dst_ext);
    put64(tk + 27, c->ext_addr);
    uint8_t st = send_aps_command_plain(c, dst, tk, sizeof tk);
    wipe(tk, sizeof tk);
    if (st != ZB_SEND_OK) { c->security.network_keys[alt].valid = false; return false; }
    for (unsigned i = 0; i < 16; i++) c->pending_key[i] = key[i];
    c->pending_key_seq = seq;
    c->pending_key_valid = true;
    c->stats.key_updates++;
    return true;
}

bool zb_coord_switch_network_key(zb_coord_t *c, uint16_t dst, uint8_t seq) {
    if (!c->pending_key_valid || seq != c->pending_key_seq || c->key_switch_due) return false;
    const uint8_t sk[2] = {APS_CMD_SWITCH_KEY, seq};
    if (send_aps_command_plain(c, dst, sk, sizeof sk) != ZB_SEND_OK) return false;
    c->key_switch_due = true;
    c->key_switch_at = c->now_ms + ZB_KEY_SWITCH_MS;
    return true;
}

static void key_switch_service(zb_coord_t *c) {
    if (!c->key_switch_due || (int32_t)(c->now_ms - c->key_switch_at) < 0) return;
    c->key_switch_due = false;
    c->tx_key_slot = (uint8_t)(1u - c->tx_key_slot);   /* the alternate slot holds the new key */
    for (unsigned i = 0; i < 16; i++) c->network_key[i] = c->pending_key[i];
    c->key_seq = c->pending_key_seq;
    wipe(c->pending_key, sizeof c->pending_key);
    c->pending_key_valid = false;
    /* Counters first, bound to the new key, then the network record. */
    (void)save_counters(c, c->nwk_reserved, c->aps_reserved);
    save_network(c);
    c->stats.key_switches++;
}

bool zb_coord_change_channel(zb_coord_t *c, uint8_t channel) {
    if (!c->mac.started || channel < 11u || channel > 26u || c->pending_channel) return false;
    if (channel == c->mac.channel) return true;
    uint8_t update_id = (uint8_t)(c->nwk_update_id + 1u);
    uint32_t mask = 1ul << channel;
    const uint8_t req[7] = {c->zdo_seq++, (uint8_t)mask, (uint8_t)(mask >> 8), (uint8_t)(mask >> 16),
                            (uint8_t)(mask >> 24), 0xFE, update_id};
    /* To every device incl. sleepy ones (copies held for their polls). */
    if (zb_coord_send_aps_ex(c, 0xFFFFu, false, 0, 0, 0x0038, 0x0000, req, sizeof req, false) != ZB_SEND_OK)
        return false;
    c->pending_channel = channel;
    c->pending_update_id = update_id;
    c->channel_change_at = c->now_ms + ZB_CHANNEL_CHANGE_MS;
    return true;
}

void zb_coord_set_update_id(zb_coord_t *c, uint8_t update_id) {
    c->nwk_update_id = update_id;
    save_network(c);
}

uint32_t zb_coord_nwk_frame_counter(const zb_coord_t *c) { return c->security.tx_next; }

bool zb_coord_raise_nwk_frame_counter(zb_coord_t *c, uint32_t value) {
    if (value <= c->security.tx_next) return true;
    /* Durable at once (a reset may follow before any frame is sent); the
     * next seal then reserves a fresh window starting at `value`. */
    if (c->store && !save_counters(c, value, c->aps_reserved)) return false;
    c->nwk_reserved = value;
    c->security.tx_next = c->security.tx_reserved = value;
    return true;
}

bool zb_coord_import_device(zb_coord_t *c, uint16_t slot_hint, uint64_t ext, uint16_t short_addr,
                            bool direct_child, const uint8_t *link_key, uint8_t tclk_shift) {
    nwk_device_t dev = {0};
    dev.ext = ext;
    dev.short_addr = short_addr;
    dev.authorized = true;
    dev.relationship = direct_child ? NWK_REL_CHILD : NWK_REL_NONE;
    /* A backup does not carry the capability: assume a listening device
     * until it polls (then it is sleepy, see on_poll) or announces itself. */
    dev.capability = NWK_CAP_RX_ON_IDLE;
    dev.capability_guessed = true;
    if (link_key) {
        for (unsigned i = 0; i < 16; i++) dev.link_key[i] = link_key[i];
        dev.unique_key = true;
        dev.tclk_shift = tclk_shift;
    }
    uint16_t slot = nwk_devices_find_ext(&c->devices, ext);
    if (slot != NWK_DEVICE_NONE) (void)zb_coord_remove_device(c, ext);
    if (slot_hint < NWK_MAX_DEVICES) {
        slot = slot_hint;
        if (c->devices.dev[slot].ext) (void)zb_coord_remove_device(c, c->devices.dev[slot].ext);
    } else if (slot == NWK_DEVICE_NONE) {
        for (slot = 0; slot < NWK_MAX_DEVICES && c->devices.dev[slot].ext; slot++) {}
    }
    /* A device owning the same short address elsewhere is stale. */
    uint16_t owner = nwk_devices_find_short(&c->devices, short_addr);
    if (owner != NWK_DEVICE_NONE) (void)zb_coord_remove_device(c, c->devices.dev[owner].ext);
    if (slot >= NWK_MAX_DEVICES || !nwk_devices_restore_slot(&c->devices, slot, &dev)) {
        wipe(&dev, sizeof dev);
        nwk_devices_rebuild_free(&c->devices);
        return false;
    }
    nwk_devices_rebuild_free(&c->devices);
    save_device(c, slot);
    wipe(&dev, sizeof dev);
    return true;
}

/* Counters record, v1 {nwk, aps} or v2 {2, nwk, aps, key check}. */
static bool parse_counters(const uint8_t *b, uint16_t len, uint32_t *nwk, uint32_t *aps) {
    le_reader_t r = le_reader(b, len);
    if (len == STORE_COUNTERS_V2_LEN && b[0] == STORE_COUNTERS_VERSION) (void)le_get_u8(&r);
    else if (len != STORE_COUNTERS_V1_LEN) return false;
    *nwk = le_get_u32(&r);
    *aps = le_get_u32(&r);
    return true;
}

/* Outgoing counters never start below a value already reserved durably,
 * WHATEVER network or key it was for: a re-formation with the same key
 * (Zigbee2MQTT keeps its network_key in its configuration) after a reset
 * of the adapter state must not restart them at zero (nonce reuse). */
static void raise_floor(zb_coord_t *c, uint32_t nwk, uint32_t aps) {
    if (nwk > c->security.tx_next) {
        c->nwk_reserved = nwk;
        c->security.tx_next = c->security.tx_reserved = nwk;
    }
    if (aps > c->tc_aps_frame_counter) c->tc_aps_frame_counter = c->aps_reserved = aps;
}

void zb_coord_attach_store(zb_coord_t *c, nv_log_t *store) {
    c->store = store;
    c->counter_floor_from_store = false;
    uint8_t b[STORE_COUNTERS_V2_LEN];
    uint16_t len = 0;
    uint32_t nwk, aps;
    if (store && nv_log_read(store, STORE_ID_COUNTERS, b, sizeof b, &len) && parse_counters(b, len, &nwk, &aps)) {
        raise_floor(c, nwk, aps);
        c->counter_floor_from_store = true;
    }
}

typedef struct { uint32_t nwk, aps; unsigned found; } salvage_t;
static void salvage_counters(void *ctx, const uint8_t *data, uint16_t len) {
    salvage_t *s = ctx;
    uint32_t nwk, aps;
    if (!parse_counters(data, len, &nwk, &aps)) return;
    if (nwk > s->nwk) s->nwk = nwk;
    if (aps > s->aps) s->aps = aps;
    s->found++;
}

bool zb_coord_store_format_keep_counters(nv_log_t *store, uint32_t *nwk_floor, uint32_t *aps_floor) {
    salvage_t s = {0, 0, 0};
    (void)nv_log_salvage(store, STORE_ID_COUNTERS, salvage_counters, &s);
    if (nwk_floor) *nwk_floor = s.nwk;
    if (aps_floor) *aps_floor = s.aps;
    if (!nv_log_format(store)) return false;
    if (!s.nwk && !s.aps) return true;
    uint8_t b[STORE_COUNTERS_V1_LEN];   /* floor only: no network, no key binding */
    le_writer_t w = le_writer(b, sizeof b);
    le_u32(&w, s.nwk);
    le_u32(&w, s.aps);
    return nv_log_write(store, STORE_ID_COUNTERS, b, (uint16_t)w.len);
}

bool zb_coord_register_endpoint(zb_coord_t *c, const zb_endpoint_t *ep) {
    if (!ep || !ep->endpoint || ep->endpoint == 0xFF || ep->in_count > ZB_MAX_EP_CLUSTERS ||
        ep->out_count > ZB_MAX_EP_CLUSTERS) return false;
    for (unsigned i = 0; i < c->endpoint_count; i++) {
        if (c->endpoints[i].endpoint == ep->endpoint) { c->endpoints[i] = *ep; return true; }
    }
    if (c->endpoint_count >= ZB_MAX_ENDPOINTS) return false;
    c->endpoints[c->endpoint_count++] = *ep;
    return true;
}

bool zb_coord_restore(zb_coord_t *c) {
    c->restore_status = ZB_RESTORE_NONE;
    if (!c->store) return false;
    uint8_t b[STORE_NET_V2_LEN];
    uint16_t len = 0;
    if (!nv_log_read(c->store, STORE_ID_NETWORK, b, sizeof b, &len)) return false;   /* no network */
    if (!((len == STORE_NET_V1_LEN && b[0] == 1u) || (len == STORE_NET_V2_LEN && b[0] == STORE_NET_VERSION))) {
        wipe(b, sizeof b);
        c->restore_status = ZB_RESTORE_NETWORK_INVALID;
        c->stats.restore_refused++;
        return false;
    }
    le_reader_t r = le_reader(b, len);
    (void)le_get_u8(&r);
    uint16_t pan = le_get_u16(&r);
    uint8_t channel = le_get_u8(&r);
    uint8_t epid[8], key[16];
    for (unsigned i = 0; i < 8; i++) epid[i] = le_get_u8(&r);
    for (unsigned i = 0; i < 16; i++) key[i] = le_get_u8(&r);
    uint8_t key_seq = le_get_u8(&r);
    bool seed_valid = false;
    uint8_t seed[16] = {0}, update_id = 0;
    if (len == STORE_NET_V2_LEN) {
        seed_valid = (le_get_u8(&r) & 1u) != 0;
        for (unsigned i = 0; i < 16; i++) seed[i] = le_get_u8(&r);
        update_id = le_get_u8(&r);
    }
    wipe(b, sizeof b);
    /* The counters bound to this key are mandatory: without them the
     * outgoing NWK/APS counters would restart at zero under the same key
     * (nonce reuse). Absent, malformed or bound to another key: refused,
     * nothing applied (recovery: restore a backup through the host). */
    uint8_t cnt[STORE_COUNTERS_V2_LEN];
    uint8_t status = ZB_RESTORE_OK;
    uint32_t nwk_upper = 0, aps_upper = 0;
    uint16_t counters_len = 0;
    if (!nv_log_read(c->store, STORE_ID_COUNTERS, cnt, sizeof cnt, &counters_len)) {
        status = ZB_RESTORE_COUNTERS_MISSING;
    } else if ((len = counters_len) == STORE_COUNTERS_V1_LEN) {        /* legacy record (no key binding) */
        le_reader_t rc = le_reader(cnt, len);
        nwk_upper = le_get_u32(&rc);
        aps_upper = le_get_u32(&rc);
    } else if (len == STORE_COUNTERS_V2_LEN && cnt[0] == STORE_COUNTERS_VERSION) {
        le_reader_t rc = le_reader(cnt, len);
        (void)le_get_u8(&rc);
        nwk_upper = le_get_u32(&rc);
        aps_upper = le_get_u32(&rc);
        if (le_get_u32(&rc) != key_check(key)) status = ZB_RESTORE_COUNTERS_OTHER_KEY;
    } else {
        status = ZB_RESTORE_COUNTERS_INVALID;
    }
    /* A legacy record was only written by the first durable reservation
     * (>= one window): zero there cannot be genuine. A v2 record is written
     * at formation before the network record, zero is then legitimate (no
     * frame secured yet: the first one reserves a window durably first). */
    if (status == ZB_RESTORE_OK && counters_len == STORE_COUNTERS_V1_LEN && nwk_upper == 0)
        status = ZB_RESTORE_COUNTERS_INCOHERENT;
    if (status != ZB_RESTORE_OK) {
        wipe(key, sizeof key);
        wipe(seed, sizeof seed);
        c->restore_status = status;
        c->stats.restore_refused++;
        return false;
    }
    c->tclk_seed_valid = seed_valid;
    for (unsigned i = 0; i < 16; i++) c->tclk_seed[i] = seed[i];
    wipe(seed, sizeof seed);
    c->nwk_update_id = update_id;
    /* Resume strictly above every counter value that may have been used. */
    core_security_init(&c->security, nwk_upper, zb_ccm_backend_active, reserve_adapter, c);
    c->nwk_reserved = nwk_upper;
    c->tc_aps_frame_counter = c->aps_reserved = aps_upper;
    for (unsigned i = 0; i < 16; i++) c->network_key[i] = key[i];
    c->key_seq = key_seq;
    (void)core_security_set_key(&c->security, 0, key_seq, key);
    wipe(key, sizeof key);
    mac_start(&c->mac, pan, channel, epid);
    nwk_devices_init(&c->devices, c->devices.rng);
    for (uint16_t slot = 0; slot < NWK_MAX_DEVICES; slot++) {
        uint8_t d[33];
        if (!nv_log_read(c->store, (uint16_t)(STORE_ID_DEVICE0 + slot), d, sizeof d, &len) || len < 12) continue;
        le_reader_t rd = le_reader(d, len);
        nwk_device_t dev = {0};
        dev.ext = le_get_u64(&rd);
        dev.short_addr = le_get_u16(&rd);
        dev.capability = le_get_u8(&rd);
        uint8_t flags = le_get_u8(&rd);
        dev.authorized = flags & 1u;
        dev.unique_key = (flags & 2u) && len >= 28;
        dev.relationship = (uint8_t)((flags >> 2) & 3u);
        dev.capability_guessed = (flags & 0x40u) != 0;
        if (dev.unique_key) {
            for (unsigned i = 0; i < 16; i++) dev.link_key[i] = le_get_u8(&rd);
            dev.tclk_shift = len >= 29 ? le_get_u8(&rd) : 0xFFu;
        }
        if ((flags & 0x80u) && rd.pos + 4u <= len) {
            dev.aps_in_counter = le_get_u32(&rd);
            dev.aps_in_seen = true;
        }
        if (nwk_devices_restore_slot(&c->devices, slot, &dev)) c->stats.restored_devices++;
        wipe(d, sizeof d);
        wipe(&dev, sizeof dev);
    }
    nwk_devices_rebuild_free(&c->devices);
    c->restore_status = ZB_RESTORE_OK;
    /* Re-write the counters in the current (v2, key-bound) format. */
    if (counters_len == STORE_COUNTERS_V1_LEN) (void)save_counters(c, c->nwk_reserved, c->aps_reserved);
    return true;
}

void zb_coord_permit_join(zb_coord_t *c, uint32_t seconds) {
    c->permit = seconds != 0;
    c->permit_until_ms = c->now_ms + seconds * 1000u;
    mac_set_association_permit(&c->mac, c->permit);
}

bool zb_coord_set_link_key(zb_coord_t *c, uint64_t ext, const uint8_t key[16]) {
    int free_slot = -1;
    for (unsigned i = 0; i < ZB_LINK_KEYS; i++) {
        if (c->link_keys[i].used && c->link_keys[i].ext == ext) { free_slot = (int)i; break; }
        if (!c->link_keys[i].used && free_slot < 0) free_slot = (int)i;
    }
    if (free_slot < 0) return false;
    c->link_keys[free_slot].used = true;
    c->link_keys[free_slot].ext = ext;
    for (unsigned i = 0; i < 16; i++) c->link_keys[free_slot].key[i] = key[i];
    return true;
}

void zb_coord_tick(zb_coord_t *c, uint32_t now_ms) {
    c->now_ms = now_ms;
    c->routes.now_ms = now_ms;
    if (c->permit && (int32_t)(now_ms - c->permit_until_ms) >= 0) zb_coord_permit_join(c, 0);
    mac_tick(&c->mac, now_ms);
    if (!c->mac.started) return;
    bcast_service(c);
    disc_service(c);
    recent_service(c);
    frag_service(c);
    key_switch_service(c);
    if (c->pending_channel && (int32_t)(now_ms - c->channel_change_at) >= 0) {
        uint8_t channel = c->pending_channel;
        c->pending_channel = 0;
        zb_coord_set_network(c, c->mac.pan_id, channel, c->mac.beacon.ext_pan_id, c->pending_update_id);
        nwk_routes_clear(&c->routes);   /* links are re-learnt on the new channel */
        c->stats.channel_changes++;
        if (c->upper.channel_changed) c->upper.channel_changed(c->upper.ctx, channel);
    }
    if (!c->timers_armed) {
        c->timers_armed = true;
        c->next_link_status_ms = now_ms + 1000u;
        c->next_mtorr_ms = now_ms + 3000u;
    }
    if ((int32_t)(now_ms - c->next_link_status_ms) >= 0) {
        c->next_link_status_ms = now_ms + LINK_STATUS_PERIOD_MS;
        nwk_neighbors_age(&c->routes);
        c->stats.routes_expired += nwk_routes_age(&c->routes, now_ms);
        send_link_status(c);
    }
    if ((int32_t)(now_ms - c->next_mtorr_ms) >= 0) {
        c->next_mtorr_ms = now_ms + MTORR_PERIOD_MS;
        send_mtorr(c);
    }
}

void zb_coord_rx(zb_coord_t *c, const uint8_t *mpdu, uint8_t length, uint8_t lqi, int8_t rssi) {
    mac_rx(&c->mac, mpdu, length, lqi, rssi);
}

void zb_coord_tx_done(zb_coord_t *c, mac_tx_result_t result, bool frame_pending) {
    mac_tx_done(&c->mac, result, frame_pending);
}
