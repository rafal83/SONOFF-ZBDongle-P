/* SPDX-License-Identifier: MIT
 * IEEE 802.15.4 coordinator MAC (non-beacon PAN, 2.4 GHz), portable.
 *
 * Implemented:
 *  - direct transmission queue with macMaxFrameRetries = 3 on NO_ACK,
 *    CHANNEL_ACCESS_FAILURE reported without retry (802.15.4-2006 7.5.6.4);
 *  - indirect transactions for short addresses through the existing
 *    core_indirect engine (24 total / 3 per child quotas), plus a small table
 *    for extended-address transactions (association responses);
 *  - radio source-matching updates so hardware ACKs carry the right
 *    frame-pending bit; macTransactionPersistenceTime expiry;
 *  - beacon request -> Zigbee beacon, association request indication and
 *    response, data request service, orphan notification and coordinator
 *    realignment;
 *  - duplicate data/command rejection by (source, DSN) with an indexed cache;
 *  - software address/PAN filtering on top of the hardware filter.
 * Not implemented: beacon-enabled PANs, GTS, MAC security, energy/active
 * scan (needs radio support), MAC-level PAN ID conflict handling.
 *
 * Threading: one owner context calls every function; the radio adapter
 * reports TX completion through mac_tx_done() from that same context.
 */
#ifndef MAC_CORE_H
#define MAC_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core_indirect.h"
#include "core_index.h"
#include "mac_frame.h"

/* Direct transmit queue sized for the host's concurrency: zigbee-herdsman
 * sends up to 16 requests at once to a Z-Stack 3.x adapter, on top of the
 * stack's own traffic (Link Status, route discovery, relays, APS ACKs).
 * With 8, bursts overflowed and the frames were lost (production bench,
 * 20 November: 32 simultaneous reads, about half lost); Z-Stack queues up
 * to 48 NWK buffers. */
#define MAC_POOL_SLOTS 48u
/* Indirect transactions may wait 7.68 s each: short-addressed ones (data for
 * sleepy children) never take more than this many frame buffers, IEEE-
 * addressed ones (association / rejoin responses) at most MAC_EXT_INDIRECT,
 * so direct traffic (link status, routed frames, many-to-one requests)
 * always finds MAC_POOL_SLOTS - MAC_INDIRECT_SLOTS - MAC_EXT_INDIRECT. */
#define MAC_INDIRECT_SLOTS 16u
#define MAC_DIRECT_QUEUE 32u
/* Frames answering a data poll (and beacons) go before everything else: the
 * device waits for them only macMaxFrameTotalWaitTime (~30 ms) after the ACK
 * that announced them. With 4, simultaneous wake-ups (20-40 sleepy children)
 * overflowed it: frame-pending ACK, nothing sent, data delayed by a whole
 * poll period or expired (network benchmark, sleepy suite). */
#define MAC_URGENT_QUEUE 12u
#define MAC_MAX_FRAME_RETRIES 3u
/* Direct frames that failed (channel busy after CSMA-CA, or no ACK after
 * MAC_MAX_FRAME_RETRIES) go back to the END of the queue for another round,
 * up to MAC_TX_ROUNDS rounds: other frames go first and the cause (busy
 * channel, collision, interference) has time to pass. Z-Stack does the same
 * at its NWK layer (Koenkk builds: NWK_MAX_DATA_RETRIES 4, MAC retries 7):
 * in a production burst, frames failing once on a busy channel were lost. */
#define MAC_TX_ROUNDS 3u
#define MAC_PERSISTENCE_MS 7680u     /* 0x01F4 unit periods x 15.36 ms */
/* Extended-address transactions: association and rejoin responses. When a
 * router with many sleepy children dies, they all rejoin through us within a
 * few seconds: with 4, most Rejoin Responses were refused, the devices kept
 * polling with their IEEE address and the frames queued under their short
 * address expired (network benchmark, parent loss: 13 % success). */
#define MAC_EXT_INDIRECT 12u
#define MAC_DUP_ENTRIES 32u
#define MAC_DUP_WINDOW_MS 2000u
/* Largest MAC payload: short destination and source with PAN ID compression
 * (MHR 9 bytes). Frames with 64-bit addresses are checked against their real
 * header when built (mac_frame_build refuses anything above MAC_MAX_MPDU).
 * The former worst-case bound (102) refused OTA image blocks sent along a
 * source route, which Z-Stack (limit from the actual header) accepts. */
#define MAC_MAX_PAYLOAD (MAC_MAX_MPDU - 9u)

enum {
    MAC_STATUS_SUCCESS = 0x00,
    MAC_STATUS_CHANNEL_ACCESS_FAILURE = 0xE1,
    MAC_STATUS_FRAME_TOO_LONG = 0xE5,
    MAC_STATUS_INVALID_PARAMETER = 0xE8,
    MAC_STATUS_NO_ACK = 0xE9,
    MAC_STATUS_TRANSACTION_EXPIRED = 0xF0,
    MAC_STATUS_TRANSACTION_OVERFLOW = 0xF1,
    MAC_STATUS_PURGED = 0xDB,         /* local: purged by the upper layer */
    MAC_STATUS_RADIO_ERROR = 0xFE,    /* local, not an 802.15.4 code */
};
typedef enum { MAC_TX_OK = 0, MAC_TX_NO_ACK, MAC_TX_BUSY, MAC_TX_ERROR } mac_tx_result_t;

typedef struct {
    /* Start one transmission (CSMA-CA, optional ACK wait). 0 = started. */
    int (*tx)(void *ctx, const uint8_t *mpdu, uint8_t len, bool wait_ack);
    /* Set or clear the hardware frame-pending bit for one source address. */
    void (*source_match)(void *ctx, bool extended, uint64_t address, bool pending);
    void *ctx;
} mac_radio_ops_t;

typedef struct {
    void (*data_confirm)(void *ctx, uint8_t handle, uint8_t status);
    void (*data_indication)(void *ctx, const mac_frame_t *frame, uint8_t lqi, int8_t rssi);
    void (*assoc_indication)(void *ctx, uint64_t device, uint8_t capability, uint8_t lqi);
    void (*orphan_indication)(void *ctx, uint64_t device);
    /* Delivery result of an association response or realignment. */
    void (*comm_status)(void *ctx, uint64_t device, uint8_t status);
    void (*poll_indication)(void *ctx, uint16_t short_addr, uint64_t ext_addr, bool served);
    void *ctx;
    /* Optional, after ctx: outcome of every transmission attempt that asked
     * for an ACK to a short address (acked or not; busy channel excluded). */
    void (*tx_attempt)(void *ctx, uint16_t dst_short, bool acked);
} mac_upper_t;

typedef struct {
    uint8_t dst_mode;          /* MAC_ADDR_SHORT or MAC_ADDR_EXT */
    uint16_t dst_short;
    uint64_t dst_ext;
    uint8_t handle;
    bool ack_request, indirect, priority;
    const uint8_t *payload;
    uint8_t length;
    uint16_t dst_pan;          /* 0 = our PAN; another PAN: inter-PAN frame from our IEEE (direct only) */
} mac_data_request_t;

enum { SLOT_FREE = 0, SLOT_DIRECT, SLOT_INDIRECT_SHORT, SLOT_INDIRECT_EXT, SLOT_INTERNAL };

typedef struct {
    uint8_t mpdu[MAC_MAX_MPDU];
    uint8_t length, kind, handle, retries, rounds;
    bool ack, confirm, comm_status, in_flight, queued;
    uint16_t dst_short;
    uint64_t dst_ext;
    uint32_t enqueued_ms;
    core_indirect_token_t token;
} mac_slot_t;

typedef struct {
    uint32_t tx_frames, tx_retries, tx_no_ack, tx_busy, tx_error;
    uint32_t rx_frames, rx_filtered, rx_duplicates, rx_malformed;
    uint32_t beacons, assoc_requests, data_requests, polls_served, polls_empty;
    uint32_t indirect_expired, queue_full, pool_full;
    uint32_t kicks_deferred;     /* send from a confirm callback, transmitted after it */
    uint32_t tx_rounds;          /* failed direct frames re-queued for another round */
    uint32_t poll_queue_full;    /* data for a poll left queued: urgent queue full (of queue_full) */
} mac_stats_t;

typedef struct {
    mac_radio_ops_t radio;
    mac_upper_t upper;
    uint64_t ext_addr;
    uint16_t pan_id, short_addr;
    uint8_t channel, dsn, bsn;
    bool started, association_permit;
    mac_zigbee_beacon_t beacon;  /* Zigbee beacon payload fields */
    mac_slot_t slots[MAC_POOL_SLOTS];
    uint8_t direct_q[MAC_DIRECT_QUEUE], direct_head, direct_count;
    uint8_t urgent_q[MAC_URGENT_QUEUE], urgent_head, urgent_count;
    int8_t active;               /* slot being transmitted, -1 if none */
    uint8_t in_callback;         /* > 0 while an upper confirm callback runs */
    core_indirect_t indirect;
    int8_t ext_indirect[MAC_EXT_INDIRECT];
    /* Duplicate cache: indexed by source key, values = DSN + time. */
    core_index_t dup_index;
    core_index_branch_t dup_branches[MAC_DUP_ENTRIES - 1];
    uint64_t dup_keys[MAC_DUP_ENTRIES];
    uint8_t dup_occupied[(MAC_DUP_ENTRIES + 7) / 8];
    uint8_t dup_seq[MAC_DUP_ENTRIES];
    uint32_t dup_time[MAC_DUP_ENTRIES];
    uint8_t dup_next;
    uint32_t now_ms;
    mac_stats_t stats;
} mac_t;

void mac_init(mac_t *m, const mac_radio_ops_t *radio, const mac_upper_t *upper, uint64_t ext_addr);
void mac_start(mac_t *m, uint16_t pan_id, uint8_t channel, const uint8_t ext_pan_id[8]);
void mac_set_association_permit(mac_t *m, bool permit);
void mac_set_beacon_capacity(mac_t *m, bool router_capacity, bool end_device_capacity, uint8_t update_id);
/* Returns MAC_STATUS_SUCCESS when queued (confirm follows), else the error. */
uint8_t mac_data_request(mac_t *m, const mac_data_request_t *req);
uint8_t mac_association_response(mac_t *m, uint64_t device, uint16_t short_addr, uint8_t status);
uint8_t mac_orphan_response(mac_t *m, uint64_t device, uint16_t short_addr);
bool mac_purge(mac_t *m, uint8_t handle);
void mac_rx(mac_t *m, const uint8_t *mpdu, uint8_t length, uint8_t lqi, int8_t rssi);
void mac_tx_done(mac_t *m, mac_tx_result_t result, bool frame_pending);
/* Re-assert the hardware frame-pending entries of every queued indirect
 * transaction (after the radio was restarted and lost its tables). */
void mac_source_match_resync(mac_t *m);
/* Advance time: expiries, then start the next transmission if idle. */
void mac_tick(mac_t *m, uint32_t now_ms);
uint8_t mac_pending_count(mac_t *m, uint16_t short_addr);
#endif
