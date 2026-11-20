/* SPDX-License-Identifier: MIT
 * Host task: MT framing over UART0 and command dispatch. Reuses the
 * existing original parser and TX pool (firmware/src/npi.c), which are
 * single-context: only this task touches them.
 */
#include "app.h"
#include "board.h"
#include "crash_log.h"
#include "hal_aes.h"
#include "hal_sys.h"
#include "hal_uart.h"
#include "le_buf.h"
#include "mt.h"
#include "npi.h"
#include "zb_ccm_backend.h"

#ifndef FRT_BUILD_REVISION
#define FRT_BUILD_REVISION 0u
#endif
#define TASK_STATUS_MAX 8u

extern uint32_t _flash_used, _sram_used;

static StaticTask_t tcb;
static StackType_t stack[STACK_HOST];
static TaskHandle_t host_handle;
static npi_parser parser;
static npi_tx_queue txq;
static npi_frame response;
static volatile bool reset_pending;
static npi_frame znp_out;
static TaskStatus_t task_status[TASK_STATUS_MAX];
static rf_rx_frame_t frame;

void host_task_notify_from_isr(void) {
    BaseType_t woken = pdFALSE;
    if (host_handle) vTaskNotifyGiveFromISR(host_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

void host_task_wake(void) {
    if (host_handle) xTaskNotifyGive(host_handle);
}

static uint32_t now_ms(void) { return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }

static void plat_ieee(void *ctx, uint8_t out[8]) {
    (void)ctx;
    for (unsigned i = 0; i < 8; i++) out[i] = app_ieee[i];
}

static void plat_reset(void *ctx, uint8_t type) {
    (void)ctx;
    (void)type;
    reset_pending = true;
}

static int diag_info(le_writer_t *w) {
    const crash_record_t *c = crash_log_previous();
    le_u8(w, 1); /* format */
    le_u8(w, FRT_VERSION_MAJOR);
    le_u8(w, FRT_VERSION_MINOR);
    le_u8(w, FRT_VERSION_PATCH);
    le_u32(w, FRT_BUILD_REVISION);
    le_u32(w, now_ms());
    le_u8(w, (uint8_t)hal_sys_reset_source());
    le_u8(w, (uint8_t)c->kind);
    le_u32(w, c->pc);
    le_u32(w, c->lr);
    le_u32(w, c->detail);
    le_u32(w, c->cfsr);
    le_bytes(w, c->where, 16);
    le_u32(w, c->boots);
    le_u16(w, app_cpu.cpu_load_permille);
    le_u16(w, app_cpu.cpu_load_peak_permille);
    le_u8(w, (uint8_t)uxTaskGetNumberOfTasks());
    le_u32(w, app_init_status);
    le_u32(w, (uint32_t)(uintptr_t)&_sram_used);
    le_u32(w, (uint32_t)(uintptr_t)&_flash_used);
    le_u8(w, tskKERNEL_VERSION_MAJOR);
    le_u8(w, tskKERNEL_VERSION_MINOR);
    le_u8(w, tskKERNEL_VERSION_BUILD);
    le_u32(w, BOARD_UART_BAUD);
    return w->ok ? (int)w->len : MT_DIAG_BAD_PARAM;
}

static int diag_task(le_writer_t *w, const uint8_t *in, uint8_t len) {
    if (len != 1) return MT_DIAG_BAD_PARAM;
    UBaseType_t count = uxTaskGetSystemState(task_status, TASK_STATUS_MAX, 0);
    if (in[0] >= count) return MT_DIAG_BAD_PARAM;
    const TaskStatus_t *t = &task_status[in[0]];
    char name[configMAX_TASK_NAME_LEN] = {0};
    for (unsigned i = 0; i < configMAX_TASK_NAME_LEN && t->pcTaskName[i]; i++) name[i] = t->pcTaskName[i];
    le_u8(w, in[0]);
    le_u8(w, (uint8_t)count);
    le_bytes(w, name, configMAX_TASK_NAME_LEN);
    le_u8(w, (uint8_t)t->uxCurrentPriority);
    le_u8(w, (uint8_t)t->eCurrentState);
    le_u16(w, (uint16_t)t->usStackHighWaterMark);  /* words never used */
    le_u32(w, t->ulRunTimeCounter);
    return w->ok ? (int)w->len : MT_DIAG_BAD_PARAM;
}

static int diag_counters(le_writer_t *w) {
    hal_uart_stats_t u;
    hal_uart_stats(&u);
    const uint32_t values[] = {
        u.rx_bytes, u.tx_bytes, u.rx_overflow, u.hw_overrun, u.framing, u.parity, u.brk,
        u.isr_count, u.isr_max_cycles, u.rx_high_water, u.tx_high_water,
        parser.accepted, parser.bad_fcs, parser.oversized, parser.timeouts,
        parser.invalid_type, parser.resync_discarded, parser.rx_errors,
        txq.dropped, txq.dropped_normal, txq.dropped_control, txq.high_water,
        app_quorum.feeds, app_quorum.misses, app_quorum.last_missing, app_quorum.worst_consecutive,
        app_radio_frames_dropped,
    };
    le_u8(w, (uint8_t)(sizeof values / sizeof values[0]));
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) le_u32(w, values[i]);
    return w->ok ? (int)w->len : MT_DIAG_BAD_PARAM;
}

static int diag_radio_stats(le_writer_t *w) {
    rf_stats_t s;
    rf_get_stats(&s);
    const uint32_t values[] = {
        s.powerups, s.setup_failures, s.last_error_step, s.rx_frames, s.rx_crc_fail,
        s.rx_ignored, s.rx_buf_full, s.rx_overflow_restarts, s.rx_dropped_parse,
        s.tx_attempts, s.tx_ok, s.tx_no_ack, s.tx_busy, s.tx_error, s.tx_ack_pending,
        s.internal_errors, s.synth_no_lock, s.src_match_overflow, s.isr_count, s.isr_max_cycles,
        s.last_cmd_status, (uint32_t)(int32_t)s.last_rssi, (uint32_t)(int32_t)s.max_rssi,
        s.channel, (uint32_t)(int32_t)(int8_t)s.power_dbm, s.high_pa, rf_is_powered(),
        s.setup_fail_step, s.setup_fail_status, s.setup_fail_fs_status,
    };
    le_u8(w, (uint8_t)(sizeof values / sizeof values[0]));
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) le_u32(w, values[i]);
    return w->ok ? (int)w->len : MT_DIAG_BAD_PARAM;
}

static int diag_coord_stats(le_writer_t *w) {
    const app_coord_snapshot_t c = app_coord;
    const uint32_t values[] = {
        c.mac.tx_frames, c.mac.tx_retries, c.mac.tx_no_ack, c.mac.tx_busy, c.mac.tx_error,
        c.mac.rx_frames, c.mac.rx_filtered, c.mac.rx_duplicates, c.mac.rx_malformed,
        c.mac.beacons, c.mac.assoc_requests, c.mac.data_requests, c.mac.polls_served, c.mac.polls_empty,
        c.mac.indirect_expired, c.mac.queue_full, c.mac.pool_full,
        c.coord.associations_accepted, c.coord.associations_denied, c.coord.transport_keys_sent,
        c.coord.transport_key_failures, c.coord.announces, c.coord.joined, c.coord.nwk_rx,
        c.coord.nwk_malformed, c.coord.nwk_unsecured_dropped, c.coord.nwk_unknown_source,
        c.coord.nwk_auth_failures, c.coord.aps_malformed, c.coord.aps_data_up,
        c.coord.aps_auth_failures, c.coord.aps_replays, c.coord.key_requests, c.coord.link_keys_sent,
        c.coord.keys_verified, c.coord.key_verify_failures, c.coord.aps_acks_sent, c.coord.aps_acks_rx,
        c.coord.zdo_requests, c.coord.zdo_responses_sent, c.coord.aps_tx, c.coord.aps_tx_failures,
        app_aps_events_dropped,
        c.devices, c.permit, c.pan_id, c.channel,
        c.coord.store_writes, c.coord.store_failures, c.coord.restored_devices, c.restored,
    };
    le_u8(w, (uint8_t)(sizeof values / sizeof values[0]));
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) le_u32(w, values[i]);
    return w->ok ? (int)w->len : MT_DIAG_BAD_PARAM;
}

static int diag_mesh_stats(le_writer_t *w) {
    const app_coord_snapshot_t c = app_coord;
    const uint32_t values[] = {
        c.coord.nwk_commands, c.coord.link_status_rx, c.coord.link_status_tx, c.coord.route_records,
        c.coord.rreq_rx, c.coord.rrep_rx, c.coord.rreq_tx, c.coord.mtorr_tx, c.coord.network_status_rx,
        c.coord.relayed, c.coord.relay_dropped, c.coord.not_for_us, c.coord.broadcast_duplicates, c.coord.no_route,
        c.coord.update_device, c.coord.tunnels_sent, c.coord.rejoins, c.coord.tc_rejoins, c.coord.leaves,
        c.coord.timeout_requests, c.neighbors, app_znp_out_dropped, app_znp_in_refused,
        /* 23..: route repair, congestion and restarts (19 November) */
        c.coord.route_failures, c.coord.neighbors_dropped, c.coord.routes_expired, c.coord.early_mtorr,
        c.coord.early_mtorr_deferred, c.coord.neighbor_restarts, c.coord.tx_not_link_failures,
        c.coord.bcast_queue_full, c.coord.link_status_failures, c.coord.aps_duplicates,
        c.mac.queue_full, c.mac.pool_full, c.mac.tx_busy, c.mac.kicks_deferred,
        c.mac.tx_rounds, c.coord.disc_buffered, c.coord.disc_sent, c.coord.disc_expired, c.coord.rreq_coalesced,
        c.coord.aps_counter_reuse,
    };
    le_u8(w, (uint8_t)(sizeof values / sizeof values[0]));
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) le_u32(w, values[i]);
    return w->ok ? (int)w->len : MT_DIAG_BAD_PARAM;
}

static int diag_perf(le_writer_t *w) {
    const app_perf_t p = app_perf;
    zb_ccm_hw_stats_t cs;
    zb_ccm_backend_hw_stats(&cs);
    hal_aes_stats_t hs = {0};
#if FRT_HW_AES
    hal_aes_get_stats(&hs);
#endif
    const uint32_t values[] = {
        p.aes_sw_block_cycles, p.ccm_sw_frame_cycles, p.ccm_hw_frame_cycles, p.hw_aes_state,
        p.znp_last, p.znp_max, p.rx_last, p.rx_max, p.tick_max,
        p.rx_latency_last_us, p.rx_latency_max_us, p.rx_measured,
        app_cpu.cpu_load_permille, app_cpu.cpu_load_peak_permille, p.radio_stack_free_words,
        /* 15..18: lowest free stack (words) of the other tasks */
        (uint32_t)uxTaskGetStackHighWaterMark(0),
        app_supervisor_handle ? (uint32_t)uxTaskGetStackHighWaterMark(app_supervisor_handle) : 0,
        (uint32_t)uxTaskGetStackHighWaterMark(xTaskGetIdleTaskHandle()),
        0u,   /* no timer task (configUSE_TIMERS 0) */
        /* 19..26: CCM* hardware backend (zeros when software only) */
        cs.hw_ops, cs.hw_faults, cs.hw_mismatches, cs.sw_redo, cs.busy, cs.hw_audits, cs.hw_audit_failures,
        cs.hw_disabled,
        /* 27..31: AES accelerator driver */
        hs.timeouts, hs.dma_errors, hs.key_errors, hs.resets, hs.reset_timeouts,
        /* 32..36: native CCM* */
        p.ccm_hw_block_frame_cycles, cs.native_ops, cs.tag_suspect, hs.ccm_ops, hs.key_check_failures,
        /* 37..43: Green Power TempMaster transmissions */
        app_gp.tx_started, app_gp.tx_ok, app_gp.tx_failed, app_gp.tx_missed, app_gp.coord_tx_deferred,
        app_gp.channel_moves, app_gp.channel_failed,
    };
    le_u8(w, (uint8_t)(sizeof values / sizeof values[0]));
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) le_u32(w, values[i]);
    return w->ok ? (int)w->len : MT_DIAG_BAD_PARAM;
}

static int diag_store_status(le_writer_t *w) {
    const nv_log_t *s = &app_store;   /* word reads of a structure owned by the radio task */
    le_u8(w, 2);                      /* format (2: + counter floors) */
    le_u8(w, (uint8_t)s->mount_status);
    le_u8(w, s->mounted);
    le_u8(w, app_coord.restore_status);
    le_u32(w, s->generation);
    le_u32(w, s->write_offset);
    le_u32(w, s->stats.mounts);
    le_u32(w, s->stats.writes);
    le_u32(w, s->stats.compactions);
    le_u32(w, s->stats.torn_records);
    le_u32(w, s->stats.torn_headers);
    le_u32(w, s->stats.erase_failures);
    le_u32(w, s->stats.program_failures);
    le_u8(w, app_coord.counter_floor_from_store);
    le_u32(w, app_counter_floor[0]);   /* kept by the last recovery format */
    le_u32(w, app_counter_floor[1]);
    return w->ok ? (int)w->len : MT_DIAG_BAD_PARAM;
}

static int diag_radio(uint8_t cmd, const uint8_t *in, uint8_t len, le_writer_t *w) {
    radio_request_t req = {0};
    if (cmd == DIAG_STORE_FORMAT) {
        if (len != 4 || in[0] != 'W' || in[1] != 'I' || in[2] != 'P' || in[3] != 'E') return MT_DIAG_BAD_PARAM;
        req.kind = RADIO_REQ_STORE_FORMAT;
    } else if (cmd == DIAG_APS_SEND) {
        if (len < 9 || len - 9u > RF_MAX_MPDU_NO_FCS) return MT_DIAG_BAD_PARAM;
        req.kind = RADIO_REQ_APS_SEND;
        req.dst_short = (uint16_t)(in[0] | (in[1] << 8));
        req.dst_ep = in[2];
        req.src_ep = in[3];
        req.cluster = (uint16_t)(in[4] | (in[5] << 8));
        req.profile = (uint16_t)(in[6] | (in[7] << 8));
        req.wait_ack = in[8];
        req.length = (uint8_t)(len - 9u);
        for (unsigned i = 0; i < req.length; i++) req.mpdu[i] = in[9 + i];
    } else if (cmd == DIAG_COORD_PERMIT) {
        if (len != 1) return MT_DIAG_BAD_PARAM;
        req.kind = RADIO_REQ_PERMIT;
        req.permit = in[0];
    } else if (cmd == DIAG_COORD_START) {
        if (len != 5 && len != 6) return MT_DIAG_BAD_PARAM;
        req.flags = len == 6 ? in[5] : 0;
        req.kind = RADIO_REQ_COORD_START;
        req.channel = in[0];
        req.power_dbm = (int8_t)in[1];
        req.pan_id = (uint16_t)(in[2] | (in[3] << 8));
        req.permit = in[4];
    } else if (cmd == DIAG_RADIO_START) {
        if (len != 3) return MT_DIAG_BAD_PARAM;
        req.kind = RADIO_REQ_START;
        req.channel = in[0];
        req.power_dbm = (int8_t)in[1];
        req.promiscuous = in[2];
    } else if (cmd == DIAG_RADIO_STOP) {
        if (len) return MT_DIAG_BAD_PARAM;
        req.kind = RADIO_REQ_STOP;
    } else {
        if (len < 4 || len - 1u > RF_MAX_MPDU_NO_FCS) return MT_DIAG_BAD_PARAM;
        req.kind = RADIO_REQ_TX;
        req.wait_ack = in[0];
        req.length = (uint8_t)(len - 1u);
        for (unsigned i = 0; i < req.length; i++) req.mpdu[i] = in[1 + i];
    }
    radio_reply_t reply;
    if (!radio_task_request(&req, &reply, 1000)) {
        le_u8(w, 0xFF);   /* radio task did not answer in time */
        return (int)w->len;
    }
    le_u8(w, (uint8_t)reply.status);
    if (cmd == DIAG_RADIO_TX) {
        le_u8(w, reply.tx_result);
        le_u8(w, reply.frame_pending);
    }
    return (int)w->len;
}

static int plat_diag(void *ctx, uint8_t cmd, const uint8_t *in, uint8_t len, uint8_t *out, uint8_t cap) {
    (void)ctx;
    le_writer_t w = le_writer(out, cap);
    switch (cmd) {
    case DIAG_INFO: return len ? MT_DIAG_BAD_PARAM : diag_info(&w);
    case DIAG_TASK: return diag_task(&w, in, len);
    case DIAG_COUNTERS: return len ? MT_DIAG_BAD_PARAM : diag_counters(&w);
    case DIAG_RADIO_STATS: return len ? MT_DIAG_BAD_PARAM : diag_radio_stats(&w);
    case DIAG_COORD_STATS: return len ? MT_DIAG_BAD_PARAM : diag_coord_stats(&w);
    case DIAG_MESH_STATS: return len ? MT_DIAG_BAD_PARAM : diag_mesh_stats(&w);
    case DIAG_STORE_STATUS: return len ? MT_DIAG_BAD_PARAM : diag_store_status(&w);
    case DIAG_PERF: return len ? MT_DIAG_BAD_PARAM : diag_perf(&w);
    case DIAG_STORE_FORMAT: {
        int n = diag_radio(cmd, in, len, &w);
        if (n > 0 && out[0] == 0) reset_pending = true;   /* boot on the empty store */
        return n;
    }
    case DIAG_APS_SEND:
    case DIAG_COORD_PERMIT:
    case DIAG_COORD_START:
    case DIAG_RADIO_START:
    case DIAG_RADIO_STOP:
    case DIAG_RADIO_TX: return diag_radio(cmd, in, len, &w);
    default: return MT_DIAG_UNKNOWN;
    }
}

static const mt_platform_t platform = {
    .context = 0,
    .ieee = plat_ieee,
    .reset = plat_reset,
    .diag = plat_diag,
    .build_revision = FRT_BUILD_REVISION,
};

void host_task_request_reset(void) {
    reset_pending = true;
    host_task_wake();
}

static void on_frame(void *context, const npi_frame *request) {
    (void)context;
    /* Private diagnostics stay here; every ZNP subsystem goes to the radio
     * task, which owns the coordinator. */
    if ((request->cmd0 & 0x1Fu) == MT_SUBSYS_FRT_DIAG) {
        if (mt_dispatch(&platform, request, &response)) (void)npi_tx_enqueue(&txq, &response);
        return;
    }
    /* herdsman does not handle MT RPC errors (it waits 6 s for the SRSP):
     * wait for room rather than refuse; it sends one request at a time. */
    if (xQueueSend(app_znp_in, request, pdMS_TO_TICKS(2000)) == pdPASS) {
        radio_task_znp_notify();
    } else {
        /* Stack stalled for 2 s: MT RPC error (memory), counted. */
        app_znp_in_refused++;
        response.cmd0 = MT_TYPE_SRSP | MT_SUBSYS_RPC_ERR;
        response.cmd1 = 0;
        response.length = 3;
        response.payload[0] = 4;
        response.payload[1] = request->cmd0;
        response.payload[2] = request->cmd1;
        if ((request->cmd0 & 0xE0u) == MT_TYPE_SREQ) (void)npi_tx_enqueue(&txq, &response);
    }
}

static void forward_znp(void) {
    /* Frames are only removed from the queue once the TX pool took them. */
    for (unsigned i = 0; i < ZNP_OUT_QUEUE; i++) {
        if (xQueuePeek(app_znp_out, &znp_out, 0) != pdPASS) return;
        if (!npi_tx_enqueue(&txq, &znp_out)) return;
        (void)xQueueReceive(app_znp_out, &znp_out, 0);
    }
}

static void forward_radio_frames(void) {
    /* Bounded: at most the queue depth per pass, and never more than the
     * normal TX lane can take (drops are counted by the TX pool). */
    for (unsigned i = 0; i < RADIO_FRAME_QUEUE; i++) {
        if (xQueueReceive(app_radio_frames, &frame, 0) != pdPASS) return;
        uint8_t payload[11 + RF_MAX_MPDU_NO_FCS];
        le_writer_t w = le_writer(payload, sizeof payload);
        le_u8(&w, frame.length);
        le_u8(&w, frame.lqi);
        le_u8(&w, (uint8_t)frame.rssi);
        le_u8(&w, frame.correlation);
        le_u8(&w, (uint8_t)(frame.crc_ok | (frame.ignored << 1)));
        le_u32(&w, frame.timestamp);
        le_u16(&w, 0);    /* reserved */
        le_bytes(&w, frame.mpdu, frame.length);
        (void)npi_tx_enqueue_fields(&txq, MT_TYPE_AREQ | MT_SUBSYS_FRT_DIAG, DIAG_RADIO_FRAME,
                                    payload, (uint8_t)w.len, NPI_TX_NORMAL);
    }
}

static void forward_join_events(void) {
    app_join_event_t e;
    while (xQueueReceive(app_join_events, &e, 0) == pdPASS) {
        uint8_t p[12];
        le_writer_t w = le_writer(p, sizeof p);
        le_u16(&w, e.short_addr);
        for (unsigned i = 0; i < 8; i++) le_u8(&w, (uint8_t)(e.ext >> (8u * i)));
        le_u8(&w, e.capability);
        le_u8(&w, e.kind);
        (void)npi_tx_enqueue_fields(&txq, MT_TYPE_AREQ | MT_SUBSYS_FRT_DIAG, DIAG_JOIN_EVENT,
                                    p, (uint8_t)w.len, NPI_TX_NORMAL);
    }
    static app_aps_event_t a;
    while (xQueueReceive(app_aps_events, &a, 0) == pdPASS) {
        uint8_t p[12 + APS_EVENT_MAX];
        le_writer_t w = le_writer(p, sizeof p);
        le_u16(&w, a.src);
        le_u8(&w, a.src_ep);
        le_u8(&w, a.dst_ep);
        le_u16(&w, a.cluster);
        le_u16(&w, a.profile);
        le_u8(&w, a.lqi);
        le_u8(&w, a.secured);
        le_u8(&w, a.length);
        le_bytes(&w, a.payload, a.length);
        (void)npi_tx_enqueue_fields(&txq, MT_TYPE_AREQ | MT_SUBSYS_FRT_DIAG, DIAG_APS_EVENT,
                                    p, (uint8_t)w.len, NPI_TX_NORMAL);
    }
}

static void flush_tx(void) {
    const uint8_t *bytes;
    size_t n;
    /* Frames are copied whole into the UART ring: never interleaved. */
    while ((n = npi_tx_frame(&txq, &bytes)) != 0 && hal_uart_tx_free() >= n) {
        (void)hal_uart_write(bytes, n);
        npi_tx_finish_frame(&txq);
    }
}

static void host(void *arg) {
    (void)arg;
    npi_tx_init(&txq);
    npi_init(&parser, on_frame, 0);
    hal_uart_init(BOARD_UART_BAUD, host_task_notify_from_isr);
    /* SYS_RESET_IND is emitted by the ZNP layer once the stack has booted. */
    uint8_t chunk[64];
    for (;;) {
        flush_tx();
        uint32_t wait = parser.count ? NPI_TIMEOUT_MS : TASK_CHECKIN_MS;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
        wd_quorum_checkin(&app_quorum, WD_BIT_HOST);
        for (unsigned pass = 0; pass < 32; pass++) {
            if (hal_uart_take_rx_errors()) npi_rx_error(&parser);
            size_t n = hal_uart_read(chunk, sizeof chunk);
            if (!n) break;
            uint32_t t = now_ms();
            for (size_t i = 0; i < n; i++) npi_feed(&parser, chunk[i], t);
            flush_tx();
        }
        if (hal_uart_take_rx_errors()) npi_rx_error(&parser);
        npi_expire(&parser, now_ms());
        forward_znp();
        forward_join_events();
        forward_radio_frames();
        flush_tx();
        forward_znp();
        flush_tx();
        if (reset_pending && txq.count == 0 && hal_uart_tx_idle()) hal_sys_reset();
    }
}

void host_task_create(void) {
    host_handle = xTaskCreateStatic(host, "host", STACK_HOST, 0, PRIO_HOST, stack, &tcb);
    configASSERT(host_handle);
}
