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
 * NOT implemented yet: APS retries of unfragmented frames (herdsman sends
 * without APS ACK request), many-to-one requests of other concentrators.
 * Incoming NWK and APS replay counters are persisted every 1024 values
 * (NWK: 16 devices per store record): after a reboot, a replay is only
 * possible within that window. A device that joins again from scratch has
 * its NWK replay state reset (its counters restart).
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
#define ZB_RECENT_TX 32u
#define ZB_RECENT_MAX 96u
#define ZB_RECENT_RETRY1_MS 1100u
#define ZB_RECENT_RETRY2_MS 1100u
#define ZB_RECENT_RETRY3_MS 1500u
/* Destinations that answered one of our reads recently: their route works
 * both ways, so an unanswered read is a lost frame (congestion), not a route
 * to rebuild; and a failed discovery does not ban them (see recent_service). */
#define ZB_ANSWERED 16u
#define ZB_ANSWERED_MS 10000u
/* ZCL re-sends are rate limited (token bucket: one every ZB_RETX_TOKEN_MS,
 * bursts of ZB_RETX_BURST): the few answers lost in a burst are re-requested
 * at once, but a saturated channel gets no extra load that would lose more
 * of them. Network benchmark (28 routers): at 50 reads/s 2 herdsman retries
 * instead of 28; at 180/s offered, 137 reads/s served (139 without any
 * re-send, 119 with 4 re-sends/s, 122 unbounded). */
#define ZB_RETX_TOKEN_MS 1000u
#define ZB_RETX_BURST 8u

/* Last network events (diagnostics, DIAG_EVENTS): who left, who reported
 * it, re-joins, associations... answers "why did this device disappear"
 * without a host-side log. */
#define ZB_EVENTS 8u

/* Diagnostics only (routing unchanged): reads left unanswered after every
 * re-send, by destination and by the first relay of their route, and reads
 * that needed a re-send, by relay. A relay is flagged when at least 1 read in
 * 3 through it needed one (production, 21 November: shutter motors used as
 * relays lost the answers of the routers behind them). Acting on it (route
 * discovery) helped bursts in simulation but hurt a congested router
 * serving sleepy children: left to production data first. */
/* Parent_annce (Zigbee R21, ZDO 0x001F / 0x801F): after a restart the
 * coordinator lists its end-device children to the routers; a router that
 * serves one of them actively answers, and it is no longer ours. */
#define ZB_PARENT_ANNCE_BASE_MS 10000u     /* apcParentAnnounceBaseTimer */
#define ZB_PARENT_ANNCE_JITTER_MS 10000u   /* apcParentAnnounceJitterMax */
#define ZB_PARENT_ANNCE_MAX 9u             /* children per frame (8-byte IEEE each) */
#define ZB_PARENT_ANNCE_RSP_MS 60000u      /* answers accepted this long after ours */
#define ZB_CHILD_HEARD_MS 300000u          /* a child we claim was heard this recently */
#define ZB_PARENT_ANNCE_RETRY_MS 5000u     /* a frame the MAC refused (saturation): announce again */
#define ZB_PARENT_ANNCE_ATTEMPTS 3u
#define ZB_LINK_STATS 8u
#define ZB_RELAY_STATS 8u
#define ZB_RELAY_SUSPECT_LATE 6u            /* reads that needed a re-send, at least 1 in 3 */
#define ZB_RELAY_SUSPECT_MS 600000u         /* flag shown 10 min */
#define ZB_LINK_STATS_RESET_MS 1800000u     /* counts older than 30 min start over */
typedef struct { uint32_t last_ms; uint16_t dst, via, misses, answers; } zb_link_stat_t;
/* MAC transmission attempts per next hop (acked unicasts): which links lose
 * ACKs. Least recently used entry replaced; counts halved near 65535. */
#define ZB_HOP_STATS 32u
typedef struct { uint32_t last_ms; uint16_t addr, tx, no_ack; } zb_hop_stat_t;
typedef struct { uint32_t last_ms, suspect_until_ms; uint16_t addr, misses, answers, late; } zb_relay_stat_t;
enum {
    ZB_EV_LEAVE = 1,          /* NWK Leave heard; detail: options (0x20 rejoin), from: MAC source */
    ZB_EV_UPDATE_DEVICE,      /* APS Update Device; detail: status (2 = left), from: the router */
    ZB_EV_REJOIN,             /* Rejoin Request to us; detail: 1 secured, 0 Trust Center rejoin */
    ZB_EV_ASSOCIATION,        /* association accepted; detail: MAC capability */
    ZB_EV_ADDRESS_CONFLICT,   /* address claimed by another IEEE */
    ZB_EV_CHILD_AGED,         /* child silent beyond its End Device Timeout */
    ZB_EV_CHILD_MOVED,        /* child heard through a router; from: that router */
    ZB_EV_REMOVED,            /* device removed from the Trust Center table */
    ZB_EV_MEMBER_LEARNED,     /* unknown network member learnt from its secured frame */
    ZB_EV_APS_DEFAULT_KEY = 11,  /* device found securing APS with the default TC link key (once) */
    ZB_EV_APS_AUTH_FAILURE = 10, /* APS-secured frame refused; detail: key id (bits 0-1), 0x10 no link key
                               * for the source, 0x20 MIC check failed, 0x40 unusable security header;
                               * from: MAC source. Once per source per ZB_EV_APS_AUTH_HOLD_MS */
};
#define ZB_EV_APS_AUTH_HOLD_MS 1800000u
typedef struct {
    uint32_t ms;
    uint8_t ext[8];                  /* IEEE, little endian (0: unknown) */
    uint16_t short_addr, from;
    uint8_t kind, detail;
} zb_event_t;
/* End Device Timeout index of children that never sent a request (Zigbee
 * nwkEndDeviceTimeoutDefault, also Z-Stack's): 8 = 256 minutes. */
#define ZB_ED_TIMEOUT_DEFAULT 8u
#define ZB_TX_TRACK MAC_POOL_SLOTS   /* unicasts awaiting their MAC confirm: one per MAC buffer */
#define ZB_DISC_PENDING 16u          /* local frames held while their route is discovered */
/* A destination whose route discovery got no reply is not searched again
 * for a backoff (4 s, doubling up to 5 min), cleared as soon as it is heard:
 * each Route Request is rebroadcast by every router, and requests repeated
 * for devices offline for months flooded the production network (324 Route
 * Requests, 20 000 rebroadcast copies in 15 min; responses of reachable
 * routers were lost in the collisions). A dead device still costs only ~9
 * requests per 15 min; the first backoff was 30 s, which banned LIVE routers
 * whose single Route Request or Reply was lost in a busy channel (network
 * benchmark at 100/s: hundreds of commands refused at once, NWK_NO_ROUTE).
 * Destinations that answered recently are never banned (ZB_ANSWERED). */
#define ZB_RDISC_FAIL 8u
#define ZB_RDISC_BACKOFF_MS 4000u
#define ZB_RDISC_BACKOFF_MAX_MS 300000u
#define ZB_DISC_WAIT_MS 5000u        /* then confirmed NWK_NO_ROUTE (0xCD) */
#define ZB_STATUS_NWK_NO_ROUTE 0xCDu
#define ZB_STATUS_BUFFER_FULL 0x11u    /* ZStatus: local transmit queue full (the host retries) */
/* APS fragmentation (Zigbee 2.2.8.4.5), window of one block. */
#define ZB_FRAG_MAX 230u             /* whole message sent (what one ZNP frame from the host can carry) */
/* Whole message received: longer than one ZNP frame, handed to the host as
 * Z-Stack does (AF_INCOMING_MSG_EXT, then AF_DATA_RETRIEVE by blocks). */
#define ZB_FRAG_RX_MAX 512u
#define ZB_FRAG_BLOCK 64u            /* APS payload per block: fits along a source route */
#define ZB_FRAG_MAX_BLOCKS 8u
#define ZB_FRAG_RX_SESSIONS 2u
#define ZB_FRAG_WAIT_MS 3000u        /* block ACK / next block timeout */
#define ZB_FRAG_RETRIES 3u
/* A delivered message stays known this long: a block repeated because our
 * ACK of it was lost (the last one above all) is acknowledged again, never
 * delivered twice (sender: apsAckWaitDuration x apscMaxFrameRetries). */
#define ZB_FRAG_DONE_MS 10000u
/* Incoming counter floor whose write failed: the frames stay accepted (the
 * anti-replay state in RAM is exact), the write is retried at the next
 * frame of that device, at most once per ZB_FLOOR_RETRY_MS over all
 * devices. Until it succeeds, a reboot widens that device's replay window
 * beyond 1024 (back to its last durable floor). */
#define ZB_FLOOR_RETRY_MS 1000u
#define ZB_UNFRAG_MAX 82u            /* larger unicasts are fragmented */
#define ZB_STATUS_APS_NO_ACK 0xB7u
/* Channel change: the coordinator switches after the broadcast had time to
 * reach every router (nwkNetworkBroadcastDeliveryTime) and sleepy parents. */
#define ZB_CHANNEL_CHANGE_MS 10000u
/* Network key switch: our own outgoing key changes this long after the
 * Switch Key broadcast went out (its first transmission uses the old key). */
#define ZB_KEY_SWITCH_MS 2000u
typedef struct {
    bool used, done;    /* done: delivered, kept to acknowledge repeated blocks */
    uint16_t src, cluster, profile, len;
    uint8_t counter, total, next, dst_ep, src_ep, lqi;
    uint32_t due_ms;
    uint8_t data[ZB_FRAG_RX_MAX];
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
    /* Green Power frame heard on the air (NWK protocol version 3), raw. */
    void (*gpdf)(void *ctx, const uint8_t *nwk, size_t len, uint8_t mac_seq, uint8_t lqi);
    /* Inter-PAN frame (Touchlink, ZLL): MAC source (extended when src_ext),
     * its PAN, APS stub cluster/profile, ZCL payload. Never secured: it
     * changes no network state, the host decides (only while it asked). */
    void (*interpan)(void *ctx, bool src_ext, uint64_t src, uint16_t src_pan, bool broadcast, uint16_t cluster,
                     uint16_t profile, const uint8_t *zcl, size_t len, uint8_t lqi);
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
    uint32_t rreq_coalesced;     /* no new Route Request: one for that destination is pending */
    uint32_t aps_counter_reuse;  /* same source and APS counter, other content: NOT a duplicate */
    uint32_t rreq_suppressed;    /* Route Request not sent: destination in discovery backoff */
    uint32_t bcast_dup_unopened; /* broadcast copies recognised before decryption */
    uint32_t frag_rx_ok, frag_rx_dropped, frag_tx_ok, frag_tx_failed, frag_tx_retries;
    uint32_t frag_rx_reacked;         /* blocks of a delivered message acknowledged again (lost ACK) */
    uint32_t floor_store_failures;    /* incoming NWK/APS counter floor writes that failed (retried) */
    uint32_t channel_changes, key_updates, key_switches, gpdf_rx;
    uint32_t net_status_retries; /* local unicast frames re-held/retried on Network Status */
    uint32_t zcl_retx_sleepy_skipped; /* no ZCL re-send to a device known to sleep */
    uint32_t zcl_answers_dropped;     /* second answer to a re-sent ZCL read, not passed up */
    uint32_t poll_children;           /* devices found polling us: made our (sleepy) children */
    uint32_t capabilities_learnt;     /* MAC capability taken from a Node Descriptor response */
    uint32_t zcl_route_rebuilds;      /* unanswered read, nothing answered lately: route rebuilt */
    uint32_t bcast_seq_skipped;       /* NWK sequence numbers skipped: still in a broadcast table */
    uint32_t zcl_retx_deferred;       /* ZCL re-send postponed: re-send budget used up */
    uint32_t reads_unanswered;        /* ZCL reads still unanswered after every re-send */
    uint32_t relays_suspected;        /* relays flagged as losing frames (ZB_RELAY_*) */
    uint32_t parent_annce_sent, parent_annce_rsp_sent, parent_annce_children_released;
    uint32_t parent_annce_rsp_ignored;   /* answers not matching our last announce (TSN, window, status) */
    uint32_t parent_annce_send_failures; /* announce frames refused by the MAC (saturation), retried */
    uint32_t aps_default_key_rx;   /* APS frames authenticated with the default key (legacy devices) */
    uint32_t children_aged;           /* children silent beyond their End Device Timeout */
    uint32_t children_moved;          /* children heard through a router: no longer ours */
    uint32_t nwk_counters_stored, nwk_counters_restored;   /* incoming NWK counter floors (store) */
    uint32_t interpan_rx, interpan_tx;
} zb_coord_stats_t;

/* Recent unicast data frame: re-sent on Network Status (route failure),
 * and ZCL reads re-sent when unanswered (see recent_service). */
typedef struct {
    uint8_t payload[ZB_RECENT_MAX];
    uint8_t len, radius, retries, answer_cmd;
    uint16_t dst, via;                 /* via: first hop of the first send (link statistics) */
    uint32_t sent_ms;
    bool used, secured, expects_reply, answered;   /* answered: kept to drop a second answer */
    /* answer_cmd: command of the answer once answered; before that, 1 once
     * the deferral of the pending re-send was counted (zcl_retx_deferred). */
} zb_recent_tx_t;

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
        uint32_t due_ms;
        uint64_t heard;               /* heard: neighbour slots that relayed it (NWK_NEIGHBORS <= 64) */
        bool used, sleepy_copies;
    } bcast[ZB_BCAST_PENDING];
    struct { uint16_t originator, sender; uint8_t id, cost; uint32_t ms; bool used; } rdisc[ZB_ROUTE_DISCOVERIES];
    /* APS duplicate rejection: (source, APS counter) seen recently. */
    struct { uint16_t src, hash; uint8_t counter; uint32_t ms; bool used; } aps_dup[ZB_APS_DUP_TABLE];
    struct { uint16_t dst; uint32_t until_ms, backoff_ms; bool used; } rdisc_fail[ZB_RDISC_FAIL];
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
        bool used, secured, notify_confirm;
    } disc[ZB_DISC_PENDING];
    uint8_t disc_count;
    zb_recent_tx_t recent_tx[ZB_RECENT_TX];
    uint8_t recent_tx_next;
    struct { uint16_t dst; uint32_t ms; } answered[ZB_ANSWERED];
    uint8_t answered_next;
    uint8_t retx_tokens;
    uint32_t retx_refill_ms;
    bool sending_ack, retrying_recent;
    zb_event_t events[ZB_EVENTS];          /* ring, events_total % ZB_EVENTS is the next slot */
    zb_link_stat_t links[ZB_LINK_STATS];   /* destinations losing answers */
    zb_relay_stat_t relays[ZB_RELAY_STATS];
    uint32_t links_version;                /* moves at every change (diagnostics snapshot) */
    zb_hop_stat_t hops[ZB_HOP_STATS];
    bool parent_annce_pending;             /* restored network: announce our children */
    uint8_t parent_annce_tsn;
    uint8_t parent_annce_tsn_first, parent_annce_frames;   /* TSNs of our last announce: first .. first + frames - 1 */
    uint8_t parent_annce_attempts;
    uint32_t parent_annce_at_ms, parent_annce_sent_ms;
    uint32_t floor_retry_ms;               /* earliest retry of a failed floor write (0: none due) */
    uint32_t events_total;
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
/* Inter-PAN frame (Touchlink): stub NWK + APS headers, sent from our IEEE
 * to `dst_ext` (dst_ext 0: MAC broadcast 0xFFFF) on PAN `dst_pan`. The
 * radio must already be on the wanted channel (host: interPanCtl). */
uint8_t zb_coord_send_interpan(zb_coord_t *c, uint64_t dst_ext, uint16_t dst_pan, uint16_t cluster, uint16_t profile,
                               const uint8_t *zcl, uint8_t len);
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
/* Associated-device list, as Z-Stack ZNP assocRemove / assocAdd (herdsman's
 * recovery for a sleepy child that does not fetch its frames): remove = no
 * longer our child (the device stays in the Trust Center table); add =
 * child again for Z-Stack relations 1..4 (CHILD_RFD .. CHILD_FFD_RX_IDLE),
 * not a child for the others. false: unknown device or address taken. */
bool zb_coord_child_remove(zb_coord_t *c, uint64_t ext);
bool zb_coord_child_add(zb_coord_t *c, uint64_t ext, uint16_t short_addr, uint8_t relation);
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
