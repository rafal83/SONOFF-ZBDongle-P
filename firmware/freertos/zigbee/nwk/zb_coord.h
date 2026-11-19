/* SPDX-License-Identifier: MIT
 * Coordinator engine: MAC association -> NWK address -> Trust Center
 * Transport Key -> secured ZDO Device Announce -> unique TC link key
 * (Request/Verify/Confirm Key) -> application data both ways.
 * Ties together mac_core, nwk_devices, trust_center, core_security.
 *
 * Implemented:
 *  - network formation parameters, timed permit-join reflected in beacons;
 *  - association policy, Transport Key (default TCLK or install-code key);
 *  - NWK authentication with replay protection; Device Announce;
 *  - Zigbee 3.0 unique TC link key: Request Key, Transport Key (TC link
 *    key), Verify Key (HMAC check), Confirm Key; APS-secured frames with
 *    per-device APS replay protection;
 *  - APS data up (decrypted when APS-secured), APS ACK when requested,
 *    APS data down to one-hop devices (direct or indirect);
 *  - minimal ZDO server for the coordinator: NWK/IEEE address, node,
 *    active endpoint, simple and match descriptors; NOT_SUPPORTED otherwise.
 *  - persistence (optional nv_log store): network parameters and key,
 *    reserved upper bounds of the outgoing NWK and TC APS frame counters
 *    (reserved durably BEFORE use, in windows of CORE_SECURITY_RESERVATION),
 *    device table entries with their unique link keys.
 *  - mesh (concentrator, as Z-Stack): Link Status (sent every 15 s, received
 *    into the neighbour table), Many-to-One Route Request every 120 s and
 *    Route Record -> source routing, Route Request/Reply both ways (we
 *    answer for ourselves and our end-device children), Network Status,
 *    relaying of unicasts to our children / neighbours, broadcast
 *    duplicate suppression, Update Device from routers with the network
 *    key tunnelled (APS Tunnel), secured and Trust Center rejoin, Leave,
 *    End Device Timeout.
 *  - router duties: broadcasts relayed with jitter and passive-ACK retries
 *    (copies queued for sleepy children when addressed to 0xFFFF), source-
 *    routed frames relayed when we are the next relay, Route Requests of
 *    other nodes relayed (route discovery table, Route Reply forwarded hop
 *    by hop), Network Status sent on relay failure, address conflicts
 *    announced (Network Status 0x0D) and resolved for our own children
 *    (unsolicited Rejoin Response with a new address).
 * NOT implemented yet: APS retries/fragmentation/duplicate rejection, key
 * switch, many-to-one requests of other concentrators. Incoming NWK/APS replay counters are NOT persisted:
 * after a reboot an old frame from a device could be replayed once.
 */
#ifndef ZB_COORD_H
#define ZB_COORD_H
#include <stdbool.h>
#include <stdint.h>
#include "aps_frame.h"
#include "core_security.h"
#include "mac_core.h"
#include "nwk_devices.h"
#include "nwk_frame.h"
#include "nv_log.h"
#include "nwk_route.h"

#define ZB_LINK_KEYS 16u
#define ZB_NWK_RADIUS 30u
#define ZB_AWAITING_KEY 8u
#define ZB_PENDING_KEYS 4u
#define ZB_COORD_ENDPOINT 1u
#define ZB_BCAST_PENDING 6u            /* broadcasts awaiting (re)transmission */
#define ZB_ROUTE_DISCOVERIES 8u        /* route requests relayed for others */
#define ZB_PASSIVE_ACK_MS 500u
#define ZB_BCAST_RETRIES 2u            /* nwkMaxBroadcastRetries */
#define ZB_SLEEPY_BCAST_COPIES 8u
#define ZB_APS_DUP_TABLE 32u
#define ZB_TX_TRACK 16u              /* unicasts awaiting their MAC confirm */
#define ZB_DISC_PENDING 8u           /* local frames held while their route is discovered */
#define ZB_DISC_WAIT_MS 5000u        /* then confirmed NWK_NO_ROUTE (0xCD) */
#define ZB_STATUS_NWK_NO_ROUTE 0xCDu
/* APS fragmentation (Zigbee 2.2.8.4.5), window of one block. */
#define ZB_FRAG_MAX 230u             /* whole message (what one ZNP frame can carry) */
#define ZB_FRAG_BLOCK 64u            /* APS payload per block: fits along a source route */
#define ZB_FRAG_MAX_BLOCKS 8u
#define ZB_FRAG_RX_SESSIONS 2u
#define ZB_FRAG_WAIT_MS 3000u        /* block ACK / next block timeout */
#define ZB_FRAG_RETRIES 3u
#define ZB_UNFRAG_MAX 82u            /* larger unicasts are fragmented */
#define ZB_STATUS_APS_NO_ACK 0xB7u
/* Channel change: the coordinator switches after the broadcast had time to
 * reach every router (nwkNetworkBroadcastDeliveryTime) and sleepy parents. */
#define ZB_CHANNEL_CHANGE_MS 10000u
/* Network key switch: our own outgoing key changes this long after the
 * Switch Key broadcast went out (its first transmission uses the old key). */
#define ZB_KEY_SWITCH_MS 2000u
typedef struct {
    bool used;
    uint16_t src, cluster, profile, len;
    uint8_t counter, total, next, dst_ep, src_ep, lqi;
    uint32_t due_ms;
    uint8_t data[ZB_FRAG_MAX];
} zb_frag_rx_t;
typedef struct {
    bool used, waiting;
    uint16_t dst, cluster, profile, len;
    uint8_t counter, total, block, retries, handle, dst_ep, src_ep;
    uint32_t due_ms;
    uint8_t data[ZB_FRAG_MAX];
} zb_frag_tx_t;
#define ZB_EARLY_MTORR_MS 10000u     /* minimum gap between many-to-one requests */
#define ZB_APS_DUP_MS 10000u

enum {
    ZB_SEND_OK = 0,
    ZB_SEND_NO_ROUTE = 1,     /* destination is not a one-hop device */
    ZB_SEND_TOO_LONG = 2,
    ZB_SEND_SECURITY = 3,
    ZB_SEND_MAC_REFUSED = 4,
};

/* Local application endpoint (AF register), served by the ZDO server. */
#define ZB_MAX_ENDPOINTS 16u   /* herdsman registers 15 */
#define ZB_MAX_EP_CLUSTERS 16u
typedef struct {
    uint8_t endpoint, version, in_count, out_count;
    uint16_t profile, device_id;
    uint16_t in[ZB_MAX_EP_CLUSTERS], out[ZB_MAX_EP_CLUSTERS];
} zb_endpoint_t;

/* zb_coord_restore() outcome (zb_coord_t.restore_status). */
enum {
    ZB_RESTORE_NONE = 0,                /* no stored network */
    ZB_RESTORE_OK = 1,
    ZB_RESTORE_NETWORK_INVALID = 2,     /* network record malformed */
    ZB_RESTORE_COUNTERS_MISSING = 3,    /* network record without its counters */
    ZB_RESTORE_COUNTERS_INVALID = 4,    /* counters record malformed */
    ZB_RESTORE_COUNTERS_OTHER_KEY = 5,  /* counters bound to another network key */
    ZB_RESTORE_COUNTERS_INCOHERENT = 6, /* legacy record at zero: never genuinely written so */
};

typedef struct {
    void (*device_joined)(void *ctx, uint16_t short_addr, uint64_t ext, uint8_t capability);
    /* Application / ZDO-response data, already NWK- (and APS-) decrypted. */
    void (*aps_data)(void *ctx, const nwk_header_t *nwk, const aps_header_t *aps,
                     const uint8_t *payload, size_t length, uint8_t lqi);
    /* Durable NWK outgoing counter reservation (see core_security). */
    bool (*reserve_nwk_counter)(void *ctx, uint32_t upper_bound);
    /* Fresh random 128-bit key (TRNG on the MCU). */
    bool (*random_key)(void *ctx, uint8_t key[16]);
    void (*key_established)(void *ctx, uint16_t short_addr, uint64_t ext);
    /* Completion of zb_coord_send_aps() identified by its handle (MAC result
     * towards the next hop; NOT an end-to-end APS acknowledgement). */
    void (*send_confirm)(void *ctx, uint8_t handle, uint8_t mac_status);
    /* A device left (NWK Leave, or Update Device "left" from its parent). */
    void (*device_left)(void *ctx, uint16_t short_addr, uint64_t ext, bool rejoin);
    void *ctx;
    /* The network moved to `channel` (zb_coord_change_channel): retune the radio. */
    void (*channel_changed)(void *ctx, uint8_t channel);
} zb_coord_upper_t;

typedef struct {
    uint32_t associations_accepted, associations_denied;
    uint32_t transport_keys_sent, transport_key_failures;
    uint32_t announces, joined;
    uint32_t nwk_rx, nwk_malformed, nwk_unsecured_dropped, nwk_unknown_source, nwk_auth_failures;
    uint32_t aps_malformed, aps_data_up;
    uint32_t aps_auth_failures, aps_replays, key_requests, link_keys_sent, keys_verified, key_verify_failures;
    uint32_t aps_acks_sent, aps_acks_rx, zdo_requests, zdo_responses_sent, aps_tx, aps_tx_failures;
    uint32_t store_writes, store_failures, restored_devices;
    uint32_t nwk_commands, link_status_rx, link_status_tx, route_records, rreq_rx, rrep_rx, rreq_tx, mtorr_tx;
    uint32_t network_status_rx, relayed, relay_dropped, not_for_us, broadcast_duplicates, no_route;
    uint32_t update_device, tunnels_sent, rejoins, tc_rejoins, leaves, timeout_requests;
    uint32_t rebroadcasts, bcast_retries, bcast_queue_full, sleepy_bcast_copies, rreq_relayed, rrep_relayed;
    uint32_t source_relayed, address_conflicts, addresses_reassigned, network_status_tx;
    uint32_t aps_duplicates, aps_secured_tx, learned_members, restore_refused;
    uint32_t route_failures, neighbors_dropped, routes_expired, early_mtorr, relay_failures_reported;
    uint32_t link_status_failures;
    uint32_t tx_not_link_failures;   /* MAC failures other than no-ACK: routes kept */
    uint32_t neighbor_restarts;      /* complete Link Status that no longer lists us */
    uint32_t early_mtorr_deferred;   /* early many-to-one request moved to the end of the minimum gap */
    uint32_t disc_buffered, disc_sent, disc_expired;   /* frames held during route discovery */
    uint32_t frag_rx_ok, frag_rx_dropped, frag_tx_ok, frag_tx_failed, frag_tx_retries;
    uint32_t channel_changes, key_updates, key_switches;
} zb_coord_stats_t;

typedef struct {
    mac_t mac;
    nwk_devices_t devices;
    core_security_t security;
    zb_coord_upper_t upper;
    uint64_t ext_addr;
    uint8_t network_key[16], key_seq;
    bool allow_default_tclk;
    bool permit;
    uint32_t permit_until_ms, now_ms;
    uint8_t nwk_seq, aps_counter, mac_handle, zdo_seq;
    uint32_t tc_aps_frame_counter, aps_reserved, nwk_reserved;
    nv_log_t *store;                 /* optional persistence */
    bool counter_floor_from_store;   /* a counters record was found when the store was attached */
    struct { uint64_t ext; uint8_t key[16]; bool used; } link_keys[ZB_LINK_KEYS];
    /* Devices whose SUCCESSFUL association response is in flight: only
     * they receive the network key once the MAC confirms delivery. */
    uint64_t awaiting_key[ZB_AWAITING_KEY];
    /* Z-Stack 3.x compatible TC link keys: key = rot(seed, shift) ^ IEEE. */
    uint8_t tclk_seed[16];
    bool tclk_seed_valid;
    uint8_t nwk_update_id;
    zb_endpoint_t endpoints[ZB_MAX_ENDPOINTS];
    uint8_t endpoint_count;
    uint8_t last_send_handle;
    /* Unique TC link keys sent but not yet verified by the device. */
    struct { uint64_t ext; uint8_t key[16]; bool used; } pending_keys[ZB_PENDING_KEYS];
    uint8_t pending_shift[ZB_PENDING_KEYS];
    nwk_routes_t routes;
    /* Broadcast relaying with passive acknowledgement: the plaintext NWK
     * frame (radius already decremented) is re-secured at each send. */
    struct {
        uint8_t frame[MAC_MAX_PAYLOAD];
        uint8_t len, hl, sends_left, sent;
        uint16_t src;
        uint8_t seq;
        uint32_t due_ms, heard;       /* heard: neighbour slots that relayed it */
        bool used, sleepy_copies;
    } bcast[ZB_BCAST_PENDING];
    struct { uint16_t originator, sender; uint8_t id, cost; uint32_t ms; bool used; } rdisc[ZB_ROUTE_DISCOVERIES];
    /* APS duplicate rejection: (source, APS counter) seen recently. */
    struct { uint16_t src; uint8_t counter; uint32_t ms; bool used; } aps_dup[ZB_APS_DUP_TABLE];
    uint8_t aps_dup_next;
    /* Routing outcome of each pending unicast (route repair on failure). */
    struct { uint16_t dst, mac_dst, relayed_src; uint8_t handle; bool used; } tx_track[ZB_TX_TRACK];
    /* Frames held while their route is discovered (Z-Stack buffers them
     * likewise): sent with the handle already given to the host as soon
     * as a route exists, else confirmed NWK_NO_ROUTE after ZB_DISC_WAIT_MS. */
    struct {
        uint8_t payload[MAC_MAX_PAYLOAD];
        uint8_t len, radius, handle;
        uint16_t dst;
        uint32_t due_ms;
        bool used, secured;
    } disc[ZB_DISC_PENDING];
    uint8_t disc_count;
    zb_frag_rx_t frag_rx[ZB_FRAG_RX_SESSIONS];
    zb_frag_tx_t frag_tx;                  /* one fragmented transmission at a time */
    int16_t forced_handle;           /* >= 0: MAC handle imposed on the next frame */
    uint8_t tx_key_slot;             /* core_security slot of the key we send with */
    bool pending_key_valid, key_switch_due;
    uint8_t pending_key[16], pending_key_seq;
    uint32_t key_switch_at;
    uint8_t pending_channel;         /* channel change in progress (0: none) */
    uint8_t pending_update_id;
    uint32_t channel_change_at;
    bool timers_armed;
    uint8_t restore_status;          /* ZB_RESTORE_* of the last zb_coord_restore() */
    uint32_t next_link_status_ms, next_mtorr_ms, last_mtorr_ms;
    zb_coord_stats_t stats;
} zb_coord_t;

void zb_coord_init(zb_coord_t *c, const mac_radio_ops_t *radio, const zb_coord_upper_t *upper,
                   uint64_t ext_addr, uint32_t random_seed, uint32_t nwk_counter_upper);
void zb_coord_form(zb_coord_t *c, uint16_t pan_id, uint8_t channel, const uint8_t ext_pan_id[8],
                   const uint8_t network_key[16], uint8_t key_seq);
/* Persistence. attach before form/restore. restore() returns true when a
 * stored network was loaded (the coordinator is then formed with it). It is
 * REFUSED (false, restore_status says why, nothing applied, nothing erased)
 * when the network record is not accompanied by valid counters bound to its
 * key: the outgoing NWK/APS counters must never restart under the same key. */
void zb_coord_attach_store(zb_coord_t *c, nv_log_t *store);
/* Explicit recovery format of the store (operator decision): the highest
 * NWK / TC APS outgoing counters of every committed counters record found
 * in either bank (even in a corrupted store) are written back as a floor in
 * the empty store, so that no later formation or restore can go below them.
 * The floors found are returned (0 when none). */
bool zb_coord_store_format_keep_counters(nv_log_t *store, uint32_t *nwk_floor, uint32_t *aps_floor);
bool zb_coord_restore(zb_coord_t *c);
/* Open joining for `seconds` (0 closes). */
void zb_coord_permit_join(zb_coord_t *c, uint32_t seconds);
/* Install-code (or other pre-configured) link key for one device. */
bool zb_coord_set_link_key(zb_coord_t *c, uint64_t ext, const uint8_t key[16]);
/* Send APS data (APS-unsecured, NWK-secured) to a one-hop device. */
uint8_t zb_coord_send_aps(zb_coord_t *c, uint16_t dst_short, uint8_t dst_ep, uint8_t src_ep,
                          uint16_t cluster, uint16_t profile, const uint8_t *payload, uint8_t length,
                          bool ack_request);
/* Same with broadcast (dst >= 0xFFF8) or APS group (group = true, dst =
 * group id) delivery; acknowledgements are never requested then. */
uint8_t zb_coord_send_aps_ex(zb_coord_t *c, uint16_t dst, bool group, uint8_t dst_ep, uint8_t src_ep,
                             uint16_t cluster, uint16_t profile, const uint8_t *payload, uint8_t length,
                             bool ack_request);
/* Unicast APS data additionally APS-secured with the destination's TC link
 * key (host "EN_SECURITY" option). ZB_SEND_SECURITY when no key is known. */
/* Unicast too long for one frame: APS fragmentation, blocks of
 * ZB_FRAG_BLOCK acknowledged one by one; send_confirm once at the end
 * (success, or 0xB7 APS_NO_ACK / 0xCD NWK_NO_ROUTE). One at a time. */
uint8_t zb_coord_send_aps_fragmented(zb_coord_t *c, uint16_t dst, uint8_t dst_ep, uint8_t src_ep, uint16_t cluster,
                                     uint16_t profile, const uint8_t *payload, uint16_t length);
/* Move the whole network to `channel` (11..26), as Z-Stack does for a
 * Mgmt_NWK_Update_req with scan duration 0xFE: broadcast the request with
 * nwkUpdateId + 1, switch ZB_CHANNEL_CHANGE_MS later, persist. */
bool zb_coord_change_channel(zb_coord_t *c, uint8_t channel);
/* Network key rotation, as Z-Stack ZDSecMgrUpdateNwkKey / SwitchNwkKey:
 * update = install `key` (sequence `seq`) as the alternate key and
 * broadcast it (APS Transport Key, NWK-secured with the current key) to
 * `dst` (0xFFFF: every device, sleepy ones via their parents); switch =
 * broadcast APS Switch Key, then send with the new key (persisted, counters
 * re-bound). Incoming frames under the old key stay accepted until reboot.
 * Zigbee2MQTT checks its configured network_key at start: it must be
 * updated to the new key, or the next start reports a mismatch. */
bool zb_coord_update_network_key(zb_coord_t *c, uint16_t dst, uint8_t seq, const uint8_t key[16]);
bool zb_coord_switch_network_key(zb_coord_t *c, uint16_t dst, uint8_t seq);
uint8_t zb_coord_send_aps_secured(zb_coord_t *c, uint16_t dst, uint8_t dst_ep, uint8_t src_ep, uint16_t cluster,
                                  uint16_t profile, const uint8_t *payload, uint8_t length, bool ack_request);
/* Start a route discovery (broadcast Route Request) towards `dst`. */
bool zb_coord_discover_route(zb_coord_t *c, uint16_t dst);
/* Register (or replace) a local endpoint. */
bool zb_coord_register_endpoint(zb_coord_t *c, const zb_endpoint_t *ep);
/* Answer a ZDO request addressed to the coordinator itself. `req` starts
 * with the ZDO sequence number; the response (written to `out`, also
 * starting with it) is returned with its length, 0 if no answer is due. */
size_t zb_coord_zdo_local(zb_coord_t *c, uint16_t cluster, const uint8_t *req, size_t len, bool broadcast,
                          uint8_t *out, size_t cap);
/* Trust Center link key seed (Z-Stack TCLK_SEED); generated at formation
 * when absent, persisted with the network record. */
void zb_coord_set_tclk_seed(zb_coord_t *c, const uint8_t seed[16]);
void zb_coord_set_update_id(zb_coord_t *c, uint8_t update_id);
/* Outgoing NWK frame counter (next value) and its monotonic raise (restore
 * from a backup: never lowered, reserved durably before use). */
uint32_t zb_coord_nwk_frame_counter(const zb_coord_t *c);
bool zb_coord_raise_nwk_frame_counter(zb_coord_t *c, uint32_t value);
/* Insert a device known from a backup (link_key NULL: none established)
 * at `slot_hint` (NWK_DEVICE_NONE: any free slot); replaces what was there. */
bool zb_coord_import_device(zb_coord_t *c, uint16_t slot_hint, uint64_t ext, uint16_t short_addr,
                            bool direct_child, const uint8_t *link_key, uint8_t tclk_shift);
bool zb_coord_remove_device(zb_coord_t *c, uint64_t ext);
/* Persist a device entry modified in place by the caller. */
void zb_coord_device_changed(zb_coord_t *c, uint16_t slot);
void zb_coord_raise_aps_frame_counter(zb_coord_t *c, uint32_t value);
/* Change PAN / channel / extended PAN of the formed network (keeps keys). */
void zb_coord_set_network(zb_coord_t *c, uint16_t pan_id, uint8_t channel, const uint8_t ext_pan_id[8],
                          uint8_t update_id);
void zb_coord_set_network_key(zb_coord_t *c, const uint8_t key[16], uint8_t key_seq);
/* Leave the network: stored network and device entries are deleted. The
 * outgoing frame counter reservations are kept (never reused). */
void zb_coord_forget(zb_coord_t *c);
/* Unique TC link key derivation (Z-Stack 3.x scheme). */
void zb_tclk_derive(const uint8_t seed[16], uint8_t shift, uint64_t ext, uint8_t key[16]);
void zb_coord_tick(zb_coord_t *c, uint32_t now_ms);
/* Radio adapter entry points (forwarded to the MAC). */
void zb_coord_rx(zb_coord_t *c, const uint8_t *mpdu, uint8_t length, uint8_t lqi, int8_t rssi);
void zb_coord_tx_done(zb_coord_t *c, mac_tx_result_t result, bool frame_pending);
#endif
