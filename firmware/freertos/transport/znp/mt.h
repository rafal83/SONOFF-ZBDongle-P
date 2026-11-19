/* SPDX-License-Identifier: MIT
 * MT (Monitor & Test) command dispatcher of the FreeRTOS target.
 *
 * Implemented, with real behaviour:
 *   SYS_PING, SYS_VERSION, SYS_GET_EXTADDR, SYS_RESET_REQ (+ SYS_RESET_IND)
 *   FRT_DIAG (private subsystem 0x1F): info, tasks, counters, radio lab ops.
 * Every other SREQ gets the standard MT RPC error SRSP. No Z-Stack product
 * id is announced (product 0xFE = experimental, not ZStack3x0), and the ping
 * capabilities only advertise SYS.
 */
#ifndef FRT_MT_H
#define FRT_MT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "npi.h"

#define MT_TYPE_POLL 0x00u
#define MT_TYPE_SREQ 0x20u
#define MT_TYPE_AREQ 0x40u
#define MT_TYPE_SRSP 0x60u
#define MT_SUBSYS_RPC_ERR 0x00u
#define MT_SUBSYS_SYS 0x01u
#define MT_SUBSYS_FRT_DIAG 0x1Fu

#define MT_RPC_ERR_SUBSYSTEM 1u
#define MT_RPC_ERR_COMMAND_ID 2u
#define MT_RPC_ERR_PARAMETER 3u
#define MT_RPC_ERR_LENGTH 4u

#define MT_SYS_RESET_REQ 0x00u
#define MT_SYS_PING 0x01u
#define MT_SYS_VERSION 0x02u
#define MT_SYS_GET_EXTADDR 0x04u
#define MT_SYS_RESET_IND 0x80u
#define MT_CAP_SYS 0x0001u

#define FRT_PRODUCT_ID 0xFEu
#define FRT_VERSION_MAJOR 0u
#define FRT_VERSION_MINOR 2u
#define FRT_VERSION_PATCH 0u
#define FRT_TRANSPORT_REV 2u

/* FRT_DIAG commands (SREQ 0x3F xx -> SRSP 0x7F xx) */
#define DIAG_INFO 0x00u
#define DIAG_TASK 0x01u          /* index */
#define DIAG_COUNTERS 0x02u
#define DIAG_RADIO_START 0x10u   /* channel, power dBm, promiscuous */
#define DIAG_RADIO_STOP 0x11u
#define DIAG_RADIO_STATS 0x12u
#define DIAG_RADIO_TX 0x13u      /* wait_ack, mpdu... (laboratory only) */
#define DIAG_COORD_START 0x14u   /* channel, power, PAN (LE), permit s, [flags: 1 = new network] */
#define DIAG_COORD_STATS 0x15u
#define DIAG_COORD_PERMIT 0x16u  /* seconds (0 closes) */
#define DIAG_APS_SEND 0x17u      /* dst short, dst ep, src ep, cluster, profile, ack, payload */
#define DIAG_MESH_STATS 0x18u    /* routing counters (neighbours, routes, relays, Update Device...) */
#define DIAG_STORE_STATUS 0x19u  /* nv_log mount status, stats, coordinator restore status */
#define DIAG_STORE_FORMAT 0x1Au  /* "WIPE": explicit erase of the FreeRTOS store, then MCU reset */
#define DIAG_PERF 0x1Bu          /* performance counters (app_perf_t, cycles at 48 MHz) */
#define DIAG_RADIO_FRAME 0x80u   /* AREQ 0x5F 0x80: received frame */
#define DIAG_JOIN_EVENT 0x83u    /* AREQ 0x5F 0x83: short, IEEE, capability, kind (0 joined, 1 key) */
#define DIAG_APS_EVENT 0x84u     /* AREQ 0x5F 0x84: src, src ep, dst ep, cluster, profile, lqi, secured, len, data */

#define MT_DIAG_UNKNOWN (-2)
#define MT_DIAG_BAD_PARAM (-1)

typedef struct {
    void *context;
    void (*ieee)(void *ctx, uint8_t out_le[8]);
    /* Called once the request is accepted; the platform resets after the TX
     * path drained. No response frame is sent (MT AREQ semantics). */
    void (*reset)(void *ctx, uint8_t type);
    /* Fill payload, return its length, MT_DIAG_BAD_PARAM or MT_DIAG_UNKNOWN. */
    int (*diag)(void *ctx, uint8_t cmd, const uint8_t *in, uint8_t in_len,
                uint8_t *out, uint8_t out_cap);
    uint32_t build_revision;
} mt_platform_t;

/* Returns true when *response must be sent. */
bool mt_dispatch(const mt_platform_t *p, const npi_frame *request, npi_frame *response);
void mt_build_reset_ind(npi_frame *out, uint8_t reason);
/* CC26x2 RSTSRC_* to SYS_RESET_IND reason (0 power-up, 1 external, 2 watchdog). */
uint8_t mt_reset_reason(uint32_t rstsrc, bool watchdog_record);
#endif
