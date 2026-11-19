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
 * Not done: replies to bidirectional GPDs (GP Response / commissioning reply
 * transmitted in the GPD's receive window): such GPDs still need a proxy
 * able to transmit them (they are commissioned through it).
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

typedef struct {
    uint32_t src_id, counter, seen_ms;
    uint8_t key[16];
    uint8_t sec_level, key_type, mac_seq;
    bool used, key_valid, counter_valid;
} gp_entry_t;

typedef struct {
    gp_entry_t table[GP_TABLE];
    bool commissioning;
    uint32_t commissioning_until;    /* 0: until exit */
    uint8_t zcl_seq;
    nv_log_t *store;
    struct {
        uint32_t gpdf_rx, malformed, forwarded, commissioning_fwd, not_paired, auth_failed, replays, duplicates;
        uint32_t pairings, removals;
    } stats;
} gp_proxy_t;

/* GPD frame (Zigbee GP 14-0563 A.1.4.1), application id 0 (SrcID). */
typedef struct {
    uint8_t frame_type, sec_level, key_type, cmd;
    bool auto_commissioning, rx_after_tx, direction;
    uint32_t src_id, counter;
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
#endif
