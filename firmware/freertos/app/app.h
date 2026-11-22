/* SPDX-License-Identifier: MIT
 * Task layout of the FreeRTOS target (see docs/firmware/architecture.md).
 *
 *   prio 5  radio    owns the RF core driver, CPE events, TX chains
 *   prio 4  timers   FreeRTOS daemon (software timers)
 *   prio 3  host     UART/MT transport, command dispatch
 *   prio 1  superv   watchdog quorum, CPU/stack statistics
 *   prio 0  idle     WFI
 * Zigbee stack tasks will be inserted between radio and host.
 */
#ifndef FRT_APP_H
#define FRT_APP_H
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "wd_quorum.h"
#include "rf_ieee.h"
#include "mac_core.h"
#include "zb_coord.h"
#include "nv_log.h"

#define PRIO_RADIO 5
#define PRIO_HOST 3
#define PRIO_SUPERVISOR 1

#define STACK_RADIO 1280u     /* words; runs MAC + NWK security + Trust Center + ZNP */
#define STACK_HOST 512u
#define STACK_SUPERVISOR 256u

enum { WD_BIT_HOST = 0, WD_BIT_RADIO = 1, WD_BIT_SUPERVISOR = 2 };

/* Longest time a monitored task may block before checking in again. */
#define TASK_CHECKIN_MS 200u
#define WATCHDOG_TIMEOUT_MS 2000u
#define SUPERVISOR_PERIOD_MS 250u

extern wd_quorum_t app_quorum;
extern uint32_t app_init_status;
extern uint8_t app_ieee[8];

/* Radio requests from the host task (blocking, bounded wait). */
typedef enum { RADIO_REQ_START, RADIO_REQ_STOP, RADIO_REQ_TX, RADIO_REQ_COORD_START, RADIO_REQ_PERMIT,
               RADIO_REQ_APS_SEND, RADIO_REQ_STORE_FORMAT } radio_req_kind_t;
typedef struct {
    radio_req_kind_t kind;
    uint8_t channel, promiscuous, wait_ack, length, permit;
    uint16_t pan_id;
    uint16_t dst_short, cluster, profile;
    uint8_t dst_ep, src_ep, flags;
    int8_t power_dbm;
    uint8_t mpdu[RF_MAX_MPDU_NO_FCS];
} radio_request_t;
typedef struct { int status; uint8_t tx_result, frame_pending; } radio_reply_t;
bool radio_task_request(const radio_request_t *req, radio_reply_t *reply, uint32_t timeout_ms);

/* Frames received in sniffer/diagnostic mode, forwarded to the host task
 * (laboratory modes only, unused in ZNP mode; 4 frames = 544 B). */
#define RADIO_FRAME_QUEUE 4u
extern QueueHandle_t app_radio_frames;
extern volatile uint32_t app_radio_frames_dropped;
/* Laboratory coordinator: join events and a counter snapshot refreshed by
 * the radio task (read word by word by the host task, may straddle an update). */
#define JOIN_EVENT_QUEUE 4u
enum { JOIN_EVENT_JOINED = 0, JOIN_EVENT_KEY_ESTABLISHED = 1 };
typedef struct { uint64_t ext; uint16_t short_addr; uint8_t capability, kind; } app_join_event_t;
/* Application data received by the coordinator, forwarded to the host. */
#define APS_EVENT_QUEUE 4u
#define APS_EVENT_MAX 80u
typedef struct {
    uint16_t src, cluster, profile;
    uint8_t src_ep, dst_ep, lqi, secured, length;
    uint8_t payload[APS_EVENT_MAX];
} app_aps_event_t;
extern QueueHandle_t app_aps_events;
extern volatile uint32_t app_aps_events_dropped;
extern QueueHandle_t app_join_events;
typedef struct {
    mac_stats_t mac;
    zb_coord_stats_t coord;
    uint16_t devices, pan_id, neighbors;
    uint8_t store_status, restore_status;   /* nv_log_mount_status_t, ZB_RESTORE_* */
    uint8_t permit, channel, restored;
    uint8_t counter_floor_from_store;
    zb_event_t events[ZB_EVENTS];   /* copied when events_total moves */
    uint32_t events_total;
    zb_link_stat_t links[ZB_LINK_STATS];     /* copied when links_version moves */
    zb_relay_stat_t relays[ZB_RELAY_STATS];
    uint32_t links_version;
    uint16_t children, sleepy_children;      /* end-device children, of them rx-off (every second) */
    uint8_t indirect_short, indirect_ext;    /* indirect frames queued now (short / IEEE destination) */
    uint8_t indirect_short_peak, indirect_ext_peak;
} app_coord_snapshot_t;
#define COORD_FLAG_NEW_NETWORK 0x01u
extern app_coord_snapshot_t app_coord;

/* ZNP frames host <-> radio task (the radio task owns the coordinator). */
#include "npi.h"
/* 4 frames each (253 B per frame): herdsman sends one request at a time and
 * waits for its SRSP; output is followed by the host task's TX queue (8
 * encoded frames) and the 1 KB UART ring. Network benchmark, 115 200 baud,
 * channel saturated: at most 4 output frames waiting, radio task never
 * blocked. 8 + 8 cost 2 KB more of the last SRAM. */
#define ZNP_IN_QUEUE 4u
#define ZNP_OUT_QUEUE 4u
extern QueueHandle_t app_znp_in, app_znp_out;
extern volatile uint32_t app_znp_out_dropped, app_znp_in_refused;
void radio_task_znp_notify(void);
void host_task_request_reset(void);

/* nv_log store owned by the radio task (read-only access for diagnostics). */
extern nv_log_t app_store;
extern uint32_t app_counter_floor[2];   /* floors kept by the last DIAG store format (0: none) */

/* Performance measured on the MCU (DWT cycle counter, 48 MHz). */
typedef struct {
    uint32_t aes_sw_block_cycles;     /* boot benchmark, mean of 16 */
    uint32_t ccm_sw_frame_cycles;     /* CCM* seal: 22-byte AAD + 40-byte payload, mean of 16 */
    uint32_t ccm_hw_frame_cycles;     /* same with the AES accelerator as used (0: not built/enabled) */
    uint32_t hw_aes_state;            /* 0 not built, 1 block mode, 2 self-test failed (software kept),
                                         3 native CCM* (block mode as fallback) */
    uint32_t znp_last, znp_max;       /* cycles of one ZNP request (SREQ -> SRSP queued, incl. AF send) */
    uint32_t rx_last, rx_max;         /* cycles of zb_coord_rx (MAC + NWK/APS security + upper) */
    uint32_t tick_max;                /* cycles of zb_coord_tick */
    uint32_t rx_latency_last_us, rx_latency_max_us, rx_measured;   /* end of frame -> processing */
    uint32_t radio_stack_free_words;  /* lowest free stack of the radio task (no heap: static allocation) */
    uint32_t ccm_hw_block_frame_cycles;   /* the same frame in block mode (ECB per block) */
} app_perf_t;
extern app_perf_t app_perf;

/* Green Power TempMaster transmissions (radio task). */
typedef struct {
    uint32_t tx_started, tx_ok, tx_failed, tx_missed, coord_tx_deferred, channel_moves, channel_failed;
} app_gp_stats_t;
extern app_gp_stats_t app_gp;

/* Touchlink sessions and radio recovery (copied from znp_t by the radio task). */
typedef struct {
    uint32_t timeouts, rx_dropped, restore_failed, restore_retries, restore_recovered, restore_restarts,
        restore_reboots, recovering;
} app_touchlink_stats_t;
extern app_touchlink_stats_t app_touchlink;

void host_task_create(void);
void radio_task_create(void);
void supervisor_task_create(void);
extern TaskHandle_t app_supervisor_handle;
void host_task_notify_from_isr(void);
void host_task_wake(void);

/* Statistics shared with the diagnostic commands. */
typedef struct {
    uint16_t cpu_load_permille;
    uint16_t cpu_load_peak_permille;
    uint32_t samples;
} app_cpu_stats_t;
extern app_cpu_stats_t app_cpu;
#endif
