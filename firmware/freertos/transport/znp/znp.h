/* SPDX-License-Identifier: MIT
 * ZNP (Z-Stack Network Processor) MT interface of the FreeRTOS coordinator,
 * as consumed by zigbee-herdsman's `zstack` adapter (Zigbee2MQTT) and zigpy-znp.
 *
 * Every answer is produced from the real coordinator state (zb_coord) and
 * the durable store (nv_log); nothing is canned. Z-Stack NV items are a
 * FACADE: small configuration items are stored as they are, while the NIB,
 * network keys, TC link key seed and the extended tables (address manager,
 * TC link keys, APS link keys, NWK security material) are SYNTHESIZED from
 * the coordinator on read and APPLIED to it on write. Their binary layouts
 * come from znp_structs.h (generated from herdsman's own struct classes).
 *
 * Implemented subsystems/commands: see docs/firmware/znp-compat.md.
 * Everything else is answered with the standard MT RPC error (unsupported
 * command), never with a fake success.
 *
 * Portable: no RTOS, no hardware. The platform supplies the output, reset,
 * randomness, radio bring-up and transmit power hooks.
 */
#ifndef ZNP_H
#define ZNP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nv_log.h"
#include "zb_coord.h"
#include "gp_proxy.h"

#define ZNP_PRODUCT_ZSTACK3X0 1u
#define ZNP_PRODUCT_EXPERIMENTAL 0xFEu

/* Z-Stack device states (ZDO stateChangeInd / getDeviceInfo). */
enum { ZNP_DEV_HOLD = 0, ZNP_DEV_COORD_STARTING = 8, ZNP_DEV_ZB_COORD = 9 };

#define ZNP_MAX_GROUPS 16u
#define ZNP_MAX_CONFIRMS 32u   /* > MAC queue + herdsman concurrency */
#define ZNP_APSKEY_CAPACITY 16u
#define ZNP_SM_CAPACITY 16u
#define ZNP_SECMAT_CAPACITY 2u
#define ZNP_NV_GENERIC_BASE 256u    /* nv_log id of generic item 0 (items 0..255) */

typedef struct {
    void *ctx;
    /* Emit one MT frame (cmd0 = type | subsystem). */
    void (*send)(void *ctx, uint8_t cmd0, uint8_t cmd1, const uint8_t *payload, uint8_t len);
    /* SYS_RESET_REQ accepted: reboot (the platform calls znp_boot again). */
    void (*reset)(void *ctx);
    bool (*random)(void *ctx, uint8_t out[16]);
    /* Bring the coordinator radio up (receive on `channel`). */
    void (*network_up)(void *ctx, uint8_t channel);
    /* Apply a transmit power, return the power actually used. */
    int8_t (*set_tx_power)(void *ctx, int8_t dbm);
    uint32_t revision;     /* SYS_VERSION revision (herdsman feature detection) */
    uint8_t product;       /* SYS_VERSION product */
    uint8_t major, minor, maint;
    /* Green Power TempMaster (optional, NULL: replies to bidirectional GPDs
     * are not transmitted). gp_tx: transmit `mpdu` (no FCS) without CSMA,
     * gpTxOffset after the END of the frame being processed (called from
     * inside the processing of that received frame). gp_channel: move the
     * receiver (and the coordinator's transmissions) to `channel`. */
    bool (*gp_tx)(void *ctx, const uint8_t *mpdu, uint8_t len);
    bool (*gp_channel)(void *ctx, uint8_t channel);
} znp_platform_t;

typedef struct {
    znp_platform_t p;
    zb_coord_t *coord;
    nv_log_t *store;
    gp_proxy_t gp;                     /* Green Power proxy (basic) */
    uint8_t gp_off_channel;            /* TempMaster channel the radio is on, 0: network channel */
    uint8_t interpan_channel;          /* Touchlink (interPanCtl): channel the radio is on, 0: none */
    uint8_t interpan_ep;               /* endpoint inter-PAN frames are delivered to, 0: none */
    uint32_t interpan_last_ms;         /* last Touchlink activity (interPanCtl, inter-PAN frame) */
    struct { uint32_t timeouts, rx_dropped; } interpan_stats;
    struct { uint32_t tx, tx_failed, channel_moves, channel_failed; } gp_stats;
    uint64_t ieee;
    uint8_t dev_state;
    bool formed;                       /* a network exists (restored or formed) */
    bool start_pending, form_pending;  /* deferred until after the SRSP */
    uint32_t primary_channels, secondary_channels;
    uint8_t zdo_seq;
    struct { uint8_t endpoint; uint16_t group; bool used; } groups[ZNP_MAX_GROUPS];
    struct { uint8_t handle, endpoint, transid; bool used; } confirms[ZNP_MAX_CONFIRMS];
    /* Security manager table written by a restore (maps APS key entries to devices). */
    uint8_t sm[2 + ZNP_SM_CAPACITY * 6u];
    bool sm_valid;
    struct {
        uint32_t frames_in, frames_unsupported, af_tx, af_tx_failed, af_rx, af_rx_dropped;
        uint32_t zdo_tx, zdo_rx, nv_writes, nv_failures;
        uint32_t restore_margins;   /* host counters raised by ZNP_RESTORE_COUNTER_MARGIN */
        uint32_t af_rx_group_any;   /* group frames delivered to endpoint 1 without membership */
    } stats;
} znp_t;

/* Outgoing frame counters written by the host during a restore (herdsman
 * adds only 2500 to the values of its backup, which may be hours or days
 * old) are raised by this margin when the store held no counters record:
 * the counters really used since the backup are unknown then. 2^22 is
 * ~80 days of the production network's average traffic, 0.1 % of the
 * 32-bit space; devices accept any higher counter. With a counters record
 * in the store, its floor already prevails (no margin). */
#define ZNP_RESTORE_COUNTER_MARGIN (1u << 22)

/* A Touchlink session (interPanCtl) idle this long ends by itself, the radio
 * back on the network channel: a host that died or failed between
 * setChannelInterPAN and restoreChannelInterPAN must not leave the whole
 * network without its coordinator. herdsman's steps are seconds apart. */
#define ZNP_INTERPAN_IDLE_MS 30000u
#define ZNP_TOUCHLINK_PROFILE 0xC05Eu   /* ZLL */
#define ZNP_TOUCHLINK_CLUSTER 0x1000u   /* ZLL commissioning */

/* Boot: (re)initialize the coordinator, apply the startup options, restore
 * the stored network, then emit SYS_RESET_IND with `reset_reason`. */
void znp_boot(znp_t *z, const znp_platform_t *platform, zb_coord_t *coord, const mac_radio_ops_t *radio,
              nv_log_t *store, uint64_t factory_ieee, uint32_t random_seed, uint8_t reset_reason);
/* One MT frame from the host. */
void znp_handle(znp_t *z, uint8_t cmd0, uint8_t cmd1, const uint8_t *payload, uint8_t len);
/* Deferred work (network start/formation); call after each znp_handle and periodically. */
void znp_poll(znp_t *z);
#endif
