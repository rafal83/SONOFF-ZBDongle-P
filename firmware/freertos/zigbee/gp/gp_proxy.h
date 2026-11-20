/* SPDX-License-Identifier: MIT
 * Green Power proxy (basic) of the coordinator: GPD frames heard directly
 * on the air become the ZCL GP (Commissioning) Notifications that a GP
 * proxy would send to the sink; zigbee-herdsman (the sink) processes them
 * exactly as those of other proxies (Hue lamps...).
 *
 *  - Commissioning frames (0xE0..0xE3) are forwarded only while the host
 *    opened commissioning (GP Commissioning Mode, sent on permit join).
 *  - Operational frames are forwarded only for GPDs the host PAIRED (GP
 *    Pairing, captured on its way out): unsecured frames for GPDs paired
 *    without security, otherwise the MIC is VERIFIED with the GPD key of the
 *    pairing and the frame counter must increase. A frame that cannot be
 *    authenticated is dropped: no forged button press reaches the host.
 *  - Paired entries are kept in the store (survive a reboot).
 *  - Bidirectional GPDs (A.3.9.1, TempMaster): a GP Response of the host
 *    naming us as TempMaster (only while commissioning, as Z-Stack) is put in
 *    the gpTxQueue (one entry, gpTxQueueEntryLifetime 5 s). When that GPD
 *    opens its receive window (GPDF with RxAfterTx, or a Channel Request),
 *    the GPDF for it is built (Commissioning Reply as a data frame, Channel
 *    Configuration as a maintenance frame) for transmission gpTxOffset
 *    (20 ms) after the end of the received frame, without CSMA. A response
 *    for another channel asks the platform to move the radio to that
 *    channel (gp_proxy_tick) until the reply is sent + 200 ms, at most 5 s;
 *    meanwhile only Channel Requests are processed.
 * Not done: GPD application id 2 (IEEE), GP sink functions, GP security
 * for frames sent to the GPD (herdsman sends its replies unsecured).
 */
#ifndef GP_PROXY_H
#define GP_PROXY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nv_log.h"

#define GP_TABLE 8u
#define GP_STORE_ID0 216u            /* nv_log ids 216..223 */
#define GP_ENDPOINT 242u
#define GP_CLUSTER 0x0021u
#define GP_DUP_MS 2000u              /* repeated unsecured GPDF (same MAC sequence) */
#define GP_TXQ_PAYLOAD 64u           /* GPD command payload of a queued reply */
#define GP_TXQ_LIFETIME_MS 5000u     /* gpTxQueueEntryLifetime / Z-Stack channel change timeout */
#define GP_RETURN_MS 200u            /* stay on the TempMaster channel after the reply */
#define GP_TX_OFFSET_US 20000u       /* gpTxOffset: end of the GPDF -> start of the reply */
#define GP_MPDU_MAX 96u

typedef struct {
    uint32_t src_id, counter, seen_ms;
    uint8_t key[16];
    uint8_t sec_level, key_type, mac_seq;
    bool used, key_valid, counter_valid;
} gp_entry_t;

/* gpTxQueue entry (A.3.6.3.1): GPDF to send to a GPD in its receive window. */
typedef struct {
    bool used, maintenance;          /* maintenance: Channel Configuration (no SrcID) */
    uint32_t src_id, expires_ms;
    uint8_t channel;                 /* 11..26, TempMasterTxChannel */
    uint8_t cmd, len, payload[GP_TXQ_PAYLOAD];
} gp_txq_t;

typedef struct {
    gp_entry_t table[GP_TABLE];
    bool commissioning;
    uint32_t commissioning_until;    /* 0: until exit */
    uint8_t zcl_seq;
    nv_log_t *store;
    /* TempMaster */
    gp_txq_t txq;
    uint16_t own_short;              /* our NWK address (coordinator: 0x0000) */
    uint8_t cur_channel;             /* channel the radio is on (set by the caller; 0: unknown) */
    uint8_t net_channel;             /* the network channel (set by the caller) */
    uint8_t want_channel;            /* 0: the network channel */
    uint32_t want_until;
    uint8_t mac_seq_tx;
    bool tx_ready;                   /* a reply was built for the frame just processed */
    uint8_t tx_len, tx_mpdu[GP_MPDU_MAX];   /* MPDU without FCS */
    struct {
        uint32_t gpdf_rx, malformed, forwarded, commissioning_fwd, not_paired, auth_failed, replays, duplicates;
        uint32_t pairings, removals;
        uint32_t responses, responses_ignored, replies, txq_expired, off_channel_dropped;
    } stats;
} gp_proxy_t;

/* GPD frame (Zigbee GP 14-0563 A.1.4.1), application id 0 (SrcID). */
typedef struct {
    uint8_t frame_type, sec_level, key_type, cmd;
    bool auto_commissioning, rx_after_tx, direction;
    uint32_t src_id, counter;        /* src_id 0: maintenance frame without SrcID */
    size_t header_len;               /* NWK FC .. frame counter */
    const uint8_t *payload;          /* after the command id */
    size_t payload_len;
    const uint8_t *mic;              /* 4 bytes for levels 2 and 3 */
} gpdf_t;

int gpdf_parse(const uint8_t *p, size_t len, gpdf_t *out);   /* 0: valid */

void gp_proxy_init(gp_proxy_t *g, nv_log_t *store);          /* loads the paired GPDs */
/* A GPD frame heard on the air: builds the ZCL frame to deliver to the host
 * (GP endpoint, cluster 0x0021); returns its length, 0 = nothing to forward. */
size_t gp_proxy_rx(gp_proxy_t *g, const uint8_t *nwk, size_t len, uint8_t mac_seq, uint32_t now_ms,
                   uint8_t *zcl, size_t cap);
/* ZCL frame the host sends on the GP endpoint (cluster 0x0021): GP Pairing
 * and GP Commissioning Mode are applied to the proxy table / state. */
void gp_proxy_host_frame(gp_proxy_t *g, const uint8_t *zcl, size_t len, uint32_t now_ms);
/* Expiry of the gpTxQueue / channel request. Returns the channel the radio
 * must be on for the TempMaster role, 0 = the network channel. */
uint8_t gp_proxy_tick(gp_proxy_t *g, uint32_t now_ms);
#endif
