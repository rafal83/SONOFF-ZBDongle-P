/* SPDX-License-Identifier: MIT
 * Radio task: sole owner of the RF core driver and of the coordinator
 * engine (MAC + join). The radio stays powered down until a laboratory DIAG
 * command; boot and UART diagnostics never depend on the RF bring-up.
 *
 * Modes:
 *   ZNP      (default) Zigbee coordinator driven by the host through the ZNP
 *            MT interface (transport/znp/znp.c): booted at power-up from
 *            the nv_log app_store, radio started by the host (startupFromApp /
 *            BDB formation). Raw-frame and lab DIAG events are not emitted.
 *   OFF      radio powered down
 *   SNIFFER  raw reception (promiscuous or filtered), synchronous test TX
 *   COORD    LABORATORY coordinator: beacons, data polls, association,
 *            Transport Key, authenticated Device Announce. The network is
 *            VOLATILE: key generated from the TRNG at start, nothing is
 *            written to Flash, everything is forgotten at reset. Joining is
 *            closed unless the host opens it with DIAG_COORD_PERMIT.
 *            Received frames are also forwarded raw to the host.
 */
#include "app.h"
#include "hal_flash.h"
#include "hal_trng.h"
#include "nv_log.h"
#include "rf_ieee.h"
#include "semphr.h"
#include "zb_coord.h"
#include "crash_log.h"
#include "hal_sys.h"
#include "mt.h"
#include "znp.h"
#include "hal_aes.h"
#include "aes128.h"
#include "hal_sys.h"
#include "zb_ccm_backend.h"

#define EVT_RF 0x1u
#define EVT_REQUEST 0x2u
#define EVT_ZNP 0x4u
#define ZNP_DEFAULT_TX_DBM 5
#define ZNP_REVISION 20261118u
#define TX_WAIT_MS 50u
#define COORD_TICK_MS 10u

typedef enum { MODE_OFF, MODE_SNIFFER, MODE_COORD, MODE_ZNP } radio_mode_t;

QueueHandle_t app_radio_frames;
QueueHandle_t app_join_events;
QueueHandle_t app_aps_events;
volatile uint32_t app_aps_events_dropped;
volatile uint32_t app_radio_frames_dropped;
app_coord_snapshot_t app_coord;
QueueHandle_t app_znp_in, app_znp_out;
app_perf_t app_perf;
volatile uint32_t app_znp_out_dropped, app_znp_in_refused;
static StaticQueue_t znp_in_queue, znp_out_queue;
static uint8_t znp_in_storage[ZNP_IN_QUEUE * sizeof(npi_frame)];
static uint8_t znp_out_storage[ZNP_OUT_QUEUE * sizeof(npi_frame)];
static znp_t znp;
static npi_frame znp_frame;      /* radio task only */
static int8_t znp_tx_dbm = ZNP_DEFAULT_TX_DBM;

static StaticTask_t tcb;
static StackType_t stack[STACK_RADIO];
static TaskHandle_t radio_handle;
static StaticQueue_t frames_queue, join_queue, aps_queue;
static uint8_t aps_storage[APS_EVENT_QUEUE * sizeof(app_aps_event_t)];
static uint8_t frames_storage[RADIO_FRAME_QUEUE * sizeof(rf_rx_frame_t)];
static uint8_t join_storage[JOIN_EVENT_QUEUE * sizeof(app_join_event_t)];

/* Single request slot; callers are serialised by the mutex. A caller that
 * timed out leaves the slot to be overwritten: the sequence number prevents
 * a late reply from being taken for the next request. */
static StaticSemaphore_t slot_mutex_buf;
static SemaphoreHandle_t slot_mutex;
static radio_request_t slot_req;
static radio_reply_t slot_reply;
static volatile uint32_t slot_seq, slot_done_seq;
static TaskHandle_t slot_requester;

static radio_mode_t mode;
static rf_rx_frame_t rx_frame;
static zb_coord_t coord;
nv_log_t app_store;
static bool store_ok;
uint32_t app_counter_floor[2];   /* NWK / TC APS floors kept by the last recovery format */
static const nv_flash_ops_t STORE_OPS = {hal_flash_read, hal_flash_program, hal_flash_erase_sector, 0};
static bool tx_active;
static TickType_t tx_started;

/* Green Power TempMaster (znp_platform_t.gp_tx / gp_channel). While a reply
 * is pending or on air, or while the radio is on another channel, the
 * coordinator's transmissions end at once as "channel busy" (a CSMA
 * failure for the NWK layer, never a link failure; the host may retry). */
#define GP_TX_ADVANCE_TICKS 0u   /* RAT ticks: TX ramp-up compensation, to calibrate with a sniffer */
#define GP_TX_WAIT_MS 100u
static uint32_t cur_rx_timestamp;   /* frame being processed by zb_coord_rx */
static uint8_t cur_rx_length;
static uint8_t gp_mpdu[GP_MPDU_MAX], gp_len;
static uint32_t gp_target;
static bool gp_pending, gp_active, gp_off_channel, tx_fake_busy;
static TickType_t gp_started;
app_gp_stats_t app_gp;

static uint32_t now_ms(void) { return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }

static void rf_notify(void) {
    BaseType_t woken = pdFALSE;
    xTaskNotifyIndexedFromISR(radio_handle, 0, EVT_RF, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

static uint64_t ieee64(void) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | app_ieee[i];
    return v;
}

/* ---- coordinator <-> radio adapter ------------------------------------ */
static int coord_radio_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx;
    if (gp_pending || gp_active || gp_off_channel) {
        /* Completed as busy by poll_tx (never from inside the MAC's kick). */
        tx_fake_busy = true;
        tx_active = true;
        tx_started = xTaskGetTickCount();
        app_gp.coord_tx_deferred++;
        return RF_OK;
    }
    int rc = rf_tx_start(mpdu, len, ack, 4);
    if (rc == RF_OK) {
        tx_active = true;
        tx_started = xTaskGetTickCount();
    }
    return rc;
}

static void coord_source_match(void *ctx, bool ext, uint64_t address, bool pending) {
    (void)ctx;
    (void)rf_source_match(ext, address, pending);
}

static void coord_joined(void *ctx, uint16_t short_addr, uint64_t ext, uint8_t capability) {
    (void)ctx;
    app_join_event_t e = {.short_addr = short_addr, .ext = ext, .capability = capability,
                          .kind = JOIN_EVENT_JOINED};
    if (xQueueSend(app_join_events, &e, 0) == pdPASS) host_task_wake();
}

static void coord_key(void *ctx, uint16_t short_addr, uint64_t ext) {
    (void)ctx;
    app_join_event_t e = {.short_addr = short_addr, .ext = ext, .kind = JOIN_EVENT_KEY_ESTABLISHED};
    if (xQueueSend(app_join_events, &e, 0) == pdPASS) host_task_wake();
}

static bool coord_random_key(void *ctx, uint8_t key[16]) {
    (void)ctx;
    return hal_trng_key(key);
}

static void coord_aps(void *ctx, const nwk_header_t *n, const aps_header_t *a, const uint8_t *p, size_t len,
                      uint8_t lqi) {
    (void)ctx;
    static app_aps_event_t e;   /* radio task only: keeps the event off the task stack */
    if (len > APS_EVENT_MAX) { app_aps_events_dropped++; return; }
    e.src = n->src;
    e.cluster = a->cluster;
    e.profile = a->profile;
    e.src_ep = a->src_endpoint;
    e.dst_ep = a->dst_endpoint;
    e.lqi = lqi;
    e.secured = a->security;
    e.length = (uint8_t)len;
    for (size_t i = 0; i < len; i++) e.payload[i] = p[i];
    if (xQueueSend(app_aps_events, &e, 0) == pdPASS) host_task_wake();
    else app_aps_events_dropped++;
}

/* Volatile laboratory network: no durable storage exists yet, so the
 * outgoing NWK counter window is accepted without persistence. */
static bool coord_reserve(void *ctx, uint32_t upper) { (void)ctx; (void)upper; return true; }

static const mac_radio_ops_t COORD_RADIO = {coord_radio_tx, coord_source_match, 0};
static const zb_coord_upper_t COORD_UPPER = {coord_joined, coord_aps, coord_reserve, coord_random_key,
                                             coord_key, 0, 0, 0, 0, 0};

/* ---- ZNP platform ------------------------------------------------------- */
static void znp_send(void *ctx, uint8_t cmd0, uint8_t cmd1, const uint8_t *payload, uint8_t len) {
    (void)ctx;
    znp_frame.cmd0 = cmd0;
    znp_frame.cmd1 = cmd1;
    znp_frame.length = len;
    for (unsigned i = 0; i < len; i++) znp_frame.payload[i] = payload[i];
    /* The host task drains continuously; a short wait keeps responses whole. */
    /* An SRSP must never be lost (the host would time out): wait up to 1 s. */
    if (xQueueSend(app_znp_out, &znp_frame, pdMS_TO_TICKS(1000)) != pdPASS) app_znp_out_dropped++;
    host_task_wake();
}

static void znp_reset(void *ctx) {
    (void)ctx;
    host_task_request_reset();
}

static bool znp_random(void *ctx, uint8_t out[16]) {
    (void)ctx;
    return hal_trng_key(out);
}

static void znp_network_up(void *ctx, uint8_t channel) {
    (void)ctx;
    if (rf_is_powered()) { tx_active = false; rf_power_down(); }
    if (rf_power_up(znp_tx_dbm)) {
        /* The 20 dBm PA setup may fail: fall back to the standard PA's
         * maximum rather than staying off the air (the actual power is what
         * stackTune reports to the host). */
        if (znp_tx_dbm <= 5 || rf_power_up(5)) return;
        znp_tx_dbm = 5;
    }
    rf_rx_config_t cfg = {
        .channel = channel, .promiscuous = false, .pan_coordinator = true,
        .pan_id = coord.mac.pan_id, .short_addr = 0x0000,
    };
    for (unsigned i = 0; i < 8; i++) cfg.ext_addr[i] = (uint8_t)(znp.ieee >> (8u * i));
    if (rf_rx_start(&cfg)) { rf_power_down(); return; }
    mode = MODE_ZNP;
    mac_source_match_resync(&coord.mac);   /* the restarted radio lost its pending-bit tables */
}

static int8_t znp_tx_power(void *ctx, int8_t dbm) {
    (void)ctx;
    if (dbm > 20) dbm = 20;
    if (dbm < -20) dbm = -20;
    znp_tx_dbm = dbm;
    /* The RF setup carries the power: herdsman tunes it once the network
     * runs, so restart the radio now (an in-flight frame ends as an error,
     * the MAC retries it). Also restarts a radio left off by a failure. */
    if (mode == MODE_ZNP) {
        if (tx_active) {
            rf_tx_abort();
            tx_active = false;
            zb_coord_tx_done(&coord, MAC_TX_ERROR, false);
        }
        znp_network_up(0, coord.mac.channel);
    }
    return znp_tx_dbm;   /* the power really applied (after a possible fallback) */
}

/* Reply to the GPD whose frame is being processed: gpTxOffset after the END
 * of that frame. The RX timestamp is taken as the start of the PHY header
 * (after the SFD); the frame lasts (PHR + MPDU + FCS) x 32 us = 128 RAT
 * ticks per byte. Both points remain to be checked with a sniffer. */
static bool radio_gp_tx(void *ctx, const uint8_t *mpdu, uint8_t len) {
    (void)ctx;
    if (gp_pending || gp_active || len > sizeof gp_mpdu || !rf_is_powered()) return false;
    for (unsigned i = 0; i < len; i++) gp_mpdu[i] = mpdu[i];
    gp_len = len;
    uint32_t end = cur_rx_timestamp + (1u + cur_rx_length + 2u) * 128u;
    gp_target = end + GP_TX_OFFSET_US * 4u - GP_TX_ADVANCE_TICKS;
    gp_pending = true;
    return true;
}

static rf_rx_config_t network_rx_config(uint8_t channel) {
    rf_rx_config_t cfg = {
        .channel = channel, .promiscuous = false, .pan_coordinator = true,
        .pan_id = coord.mac.pan_id, .short_addr = 0x0000,
    };
    for (unsigned i = 0; i < 8; i++) cfg.ext_addr[i] = (uint8_t)(znp.ieee >> (8u * i));
    return cfg;
}

/* TempMaster on another channel (Channel Configuration), then back. */
static bool radio_gp_channel(void *ctx, uint8_t channel) {
    (void)ctx;
    if (mode != MODE_ZNP || !rf_is_powered()) return false;
    if (tx_active && !tx_fake_busy) {          /* a frame in flight ends as busy */
        rf_tx_abort();
        tx_active = false;
        zb_coord_tx_done(&coord, MAC_TX_BUSY, false);
    }
    if (gp_active) { rf_tx_abort(); gp_active = false; app_gp.tx_failed++; }
    gp_pending = false;
    (void)rf_rx_stop();
    rf_rx_config_t cfg = network_rx_config(channel);
    if (rf_rx_start(&cfg)) {
        cfg = network_rx_config(coord.mac.channel);   /* never stay deaf: back to the network */
        (void)rf_rx_start(&cfg);
        gp_off_channel = false;
        app_gp.channel_failed++;
        mac_source_match_resync(&coord.mac);
        return false;
    }
    gp_off_channel = channel != coord.mac.channel;
    if (gp_off_channel) app_gp.channel_moves++;
    mac_source_match_resync(&coord.mac);   /* the restarted radio lost its pending-bit tables */
    return true;
}

/* Starts the pending reply once no coordinator frame is on air; completion. */
static void gp_service(void) {
    if (gp_active) {
        rf_tx_result_t r;
        bool pending = false;
        if (rf_tx_poll(&r, &pending)) {
            gp_active = false;
            if (r == RF_TX_DONE) app_gp.tx_ok++;
            else app_gp.tx_failed++;
        } else if (xTaskGetTickCount() - gp_started > pdMS_TO_TICKS(GP_TX_WAIT_MS)) {
            rf_tx_abort();
            gp_active = false;
            app_gp.tx_failed++;
        }
        return;
    }
    if (!gp_pending || (tx_active && !tx_fake_busy)) return;   /* a coordinator frame finishes first */
    gp_pending = false;
    if (rf_tx_at(gp_mpdu, gp_len, gp_target) == RF_OK) {
        gp_active = true;
        gp_started = xTaskGetTickCount();
        app_gp.tx_started++;
    } else {
        app_gp.tx_missed++;   /* window already too close or passed */
    }
}

static const znp_platform_t ZNP_PLATFORM = {
    .send = znp_send, .reset = znp_reset, .random = znp_random, .network_up = znp_network_up,
    .set_tx_power = znp_tx_power, .revision = ZNP_REVISION, .product = ZNP_PRODUCT_ZSTACK3X0,
    .major = 2, .minor = 7, .maint = 1,
    .gp_tx = radio_gp_tx, .gp_channel = radio_gp_channel,
};

static void poll_tx(void) {
    if (!tx_active) return;
    if (tx_fake_busy) {
        tx_fake_busy = false;
        tx_active = false;
        zb_coord_tx_done(&coord, MAC_TX_BUSY, false);
        return;
    }
    rf_tx_result_t result;
    bool pending = false;
    if (rf_tx_poll(&result, &pending)) {
        tx_active = false;
        static const mac_tx_result_t map[] = {MAC_TX_OK, MAC_TX_NO_ACK, MAC_TX_BUSY, MAC_TX_ERROR};
        zb_coord_tx_done(&coord, map[result], pending);
    } else if (xTaskGetTickCount() - tx_started > pdMS_TO_TICKS(TX_WAIT_MS)) {
        rf_tx_abort();
        tx_active = false;
        zb_coord_tx_done(&coord, MAC_TX_ERROR, false);
    }
}

static void forward(const rf_rx_frame_t *f) {
    if (xQueueSend(app_radio_frames, f, 0) != pdPASS) app_radio_frames_dropped++;
    else host_task_wake();
}

static void drain_rx(void) {
    while (rf_rx_take(&rx_frame)) {
        if (mode == MODE_OFF) continue;
        if (mode != MODE_ZNP) forward(&rx_frame);
        if ((mode == MODE_COORD || mode == MODE_ZNP) && rx_frame.crc_ok) {
            uint32_t rat = rf_rat_now();
            if (rat) {
                uint32_t us = (rat - rx_frame.timestamp) / 4u;   /* RAT: 4 MHz */
                app_perf.rx_latency_last_us = us;
                if (us > app_perf.rx_latency_max_us && us < 10000000u) app_perf.rx_latency_max_us = us;
                app_perf.rx_measured++;
            }
            uint32_t t0 = hal_cycles();
            cur_rx_timestamp = rx_frame.timestamp;
            cur_rx_length = rx_frame.length;
            zb_coord_rx(&coord, rx_frame.mpdu, rx_frame.length, rx_frame.lqi, rx_frame.rssi);
            uint32_t dt = hal_cycles() - t0;
            app_perf.rx_last = dt;
            if (dt > app_perf.rx_max) app_perf.rx_max = dt;
        }
    }
}

static void snapshot(void) {
    app_coord.mac = coord.mac.stats;
    app_coord.coord = coord.stats;
    app_coord.devices = coord.devices.count;
    app_coord.counter_floor_from_store = coord.counter_floor_from_store;
    app_coord.permit = coord.permit;
    app_coord.pan_id = coord.mac.pan_id;
    app_coord.channel = coord.mac.channel;
    uint16_t nb = 0;
    for (unsigned i = 0; i < NWK_NEIGHBORS; i++) nb += coord.routes.neighbors[i].used;
    app_coord.neighbors = nb;
    app_coord.store_status = (uint8_t)app_store.mount_status;
    app_coord.restore_status = coord.restore_status;
}


static void stop_radio(void) {
    mode = MODE_OFF;
    tx_active = false;
    rf_power_down();
    /* Forget the volatile laboratory network, including its key. */
    volatile uint8_t *wipe = (volatile uint8_t *)&coord;
    for (size_t i = 0; i < sizeof coord; i++) wipe[i] = 0;
}

static int start_radio(const radio_request_t *r, bool coordinator) {
    if (rf_is_powered()) stop_radio();
    uint8_t key[16];
    if (coordinator && !hal_trng_key(key)) return RF_ERR_PARAM;
    int rc = rf_power_up(r->power_dbm);
    if (rc) return rc;
    rf_rx_config_t cfg = {
        .channel = r->channel,
        .promiscuous = !coordinator && r->promiscuous,
        .pan_coordinator = coordinator,
        .pan_id = coordinator ? r->pan_id : 0xFFFF,
        .short_addr = coordinator ? 0x0000 : 0xFFFE,
    };
    for (unsigned i = 0; i < 8; i++) cfg.ext_addr[i] = app_ieee[i];
    if (coordinator) {
        zb_coord_init(&coord, &COORD_RADIO, &COORD_UPPER, ieee64(), hal_trng_u32(), 0);
        if (store_ok) zb_coord_attach_store(&coord, &app_store);
        bool restored = false;
        if (store_ok && (r->flags & COORD_FLAG_NEW_NETWORK))
            (void)zb_coord_store_format_keep_counters(&app_store, 0, 0);
        else if (store_ok) restored = zb_coord_restore(&coord);
        if (!restored) zb_coord_form(&coord, r->pan_id, r->channel, app_ieee, key, 0);   /* EPID = IEEE */
        app_coord.restored = restored;
        /* A restored network keeps its own channel and PAN ID. */
        cfg.channel = coord.mac.channel;
        cfg.pan_id = coord.mac.pan_id;
        zb_coord_tick(&coord, now_ms());
        if (r->permit) zb_coord_permit_join(&coord, r->permit);
    }
    volatile uint8_t *wipe = key;
    for (unsigned i = 0; i < 16; i++) wipe[i] = 0;
    rc = rf_rx_start(&cfg);
    if (rc) {
        stop_radio();
        return rc;
    }
    mode = coordinator ? MODE_COORD : MODE_SNIFFER;
    return RF_OK;
}

static radio_reply_t serve(const radio_request_t *r) {
    radio_reply_t reply = {0};
    switch (r->kind) {
    case RADIO_REQ_START:
        /* The sniffer would stop the ZNP network: only while it is down. */
        reply.status = mode == MODE_ZNP ? RF_ERR_STATE : start_radio(r, false);
        break;
    case RADIO_REQ_COORD_START:
        if (mode == MODE_ZNP || znp.formed) { reply.status = RF_ERR_STATE; break; }   /* ZNP owns it */
        reply.status = r->channel >= 11 && r->channel <= 26 && r->pan_id < 0xFFF0
                     ? start_radio(r, true) : RF_ERR_PARAM;
        break;
    case RADIO_REQ_PERMIT:
        if (mode != MODE_COORD) { reply.status = RF_ERR_STATE; break; }
        zb_coord_permit_join(&coord, r->permit);
        break;
    case RADIO_REQ_STORE_FORMAT:
        /* Explicit recovery of an unusable app_store, decided by the operator
         * (DIAG_STORE_FORMAT): everything in it is erased EXCEPT the highest
         * outgoing frame counters found in any committed counters record
         * (salvaged even from a corrupted store, rewritten as a floor); the
         * host then resets the MCU so the stack boots on the empty store. */
        if (rf_is_powered()) { tx_active = false; rf_power_down(); }
        app_store.ops = STORE_OPS;
        app_store.base = HAL_FLASH_STORE_BASE;
        app_store.bank_size = HAL_FLASH_BANK_SIZE;
        app_store.sector_size = HAL_FLASH_SECTOR;
        store_ok = zb_coord_store_format_keep_counters(&app_store, &app_counter_floor[0], &app_counter_floor[1]);
        reply.status = store_ok ? 0 : RF_ERR_STATE;
        break;
    case RADIO_REQ_APS_SEND:
        if (mode != MODE_COORD) { reply.status = RF_ERR_STATE; break; }
        reply.status = zb_coord_send_aps(&coord, r->dst_short, r->dst_ep, r->src_ep, r->cluster, r->profile,
                                         r->mpdu, r->length, r->wait_ack != 0);
        break;
    case RADIO_REQ_STOP:
        if (mode == MODE_ZNP) { reply.status = RF_ERR_STATE; break; }
        stop_radio();
        break;
    case RADIO_REQ_TX: {
        if (mode != MODE_SNIFFER) { reply.status = RF_ERR_STATE; break; }
        reply.status = rf_tx_start(r->mpdu, r->length, r->wait_ack != 0, 4);
        if (reply.status) break;
        TickType_t start = xTaskGetTickCount();
        rf_tx_result_t result = RF_TX_ERROR;
        bool pending = false;
        for (;;) {
            uint32_t bits = 0;
            (void)xTaskNotifyWaitIndexed(0, 0, EVT_RF, &bits, pdMS_TO_TICKS(5));
            (void)rf_take_events();
            drain_rx();
            if (rf_tx_poll(&result, &pending)) break;
            if (xTaskGetTickCount() - start > pdMS_TO_TICKS(TX_WAIT_MS)) {
                rf_tx_abort();
                result = RF_TX_ERROR;
                reply.status = RF_ERR_CMD_TIMEOUT;
                break;
            }
        }
        reply.tx_result = (uint8_t)result;
        reply.frame_pending = pending;
        break;
    }
    }
    return reply;
}

/* Boot benchmark of the CCM* backends on a typical frame. */
static uint32_t bench_ccm(core_security_ccm_t fn) {
    static const uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t nonce[13] = {0}, aad[22], in[40], out[40], mic[4];
    for (unsigned i = 0; i < sizeof aad; i++) aad[i] = (uint8_t)i;
    for (unsigned i = 0; i < sizeof in; i++) in[i] = (uint8_t)(3 * i);
    /* Warm-up (not measured): the one-time key load and key check are not
     * the steady-state cost of a frame. */
    nonce[1] = 0xA5;
    (void)fn(0, true, key, nonce, aad, sizeof aad, in, sizeof in, out, mic);
    nonce[1] = 0;
    uint32_t start = hal_cycles();
    for (unsigned n = 0; n < 16; n++) {
        nonce[0] = (uint8_t)n;
        (void)fn(0, true, key, nonce, aad, sizeof aad, in, sizeof in, out, mic);
    }
    return (hal_cycles() - start) / 16u;
}

static void crypto_setup(void) {
    static const uint8_t key[16] = {0};
    uint8_t blk[16] = {0};
    aes128_ctx_t a;
    aes128_init(&a, key);
    uint32_t start = hal_cycles();
    for (unsigned n = 0; n < 16; n++) aes128_encrypt(&a, blk, blk);
    app_perf.aes_sw_block_cycles = (hal_cycles() - start) / 16u;
    aes128_clear(&a);
    app_perf.ccm_sw_frame_cycles = bench_ccm(zb_ccm_backend_sw);
#if FRT_HW_AES
    /* Accelerator only after its self-test, otherwise software is kept. */
    if (hal_aes_selftest()) {
        zb_ccm_backend_set_block(hal_aes_block);
        app_perf.ccm_hw_block_frame_cycles = bench_ccm(zb_ccm_backend_hw);
        app_perf.ccm_hw_frame_cycles = app_perf.ccm_hw_block_frame_cycles;
        app_perf.hw_aes_state = 1;
        /* Native CCM* only after its own self-test against the software
         * CCM*; the block mode stays installed for what it does not cover. */
        if (hal_aes_ccm_selftest()) {
            zb_ccm_backend_set_native(hal_aes_ccm);
            app_perf.ccm_hw_frame_cycles = bench_ccm(zb_ccm_backend_hw);
            app_perf.hw_aes_state = 3;
        }
        zb_ccm_backend_active = zb_ccm_backend_hw;
    } else {
        app_perf.hw_aes_state = 2;
    }
#endif
}

static void radio(void *arg) {
    (void)arg;
    rf_init(rf_notify);
    /* Formats only a blank store; a corrupted one is kept untouched and
     * reported (DIAG_STORE_STATUS) until DIAG_STORE_FORMAT is requested. */
    store_ok = nv_log_mount(&app_store, &STORE_OPS, HAL_FLASH_STORE_BASE, HAL_FLASH_BANK_SIZE, HAL_FLASH_SECTOR);
    if (store_ok) {
        /* Finishes a migration interrupted after its commit (no-op otherwise). */
        (void)nv_log_retire_old(&app_store, HAL_FLASH_OLD_STORE_BASE, HAL_FLASH_OLD_BANK_SIZE);
    } else if (app_store.mount_status == NV_LOG_ERR_CORRUPT) {
        /* First boot after the layout change: the old store (inside the new
         * bank 1) is copied into bank 0, power-cut safe; if there is no old
         * store either, the corrupt state is kept for an explicit recovery. */
        store_ok = nv_log_migrate(&app_store, &STORE_OPS, HAL_FLASH_OLD_STORE_BASE, HAL_FLASH_OLD_BANK_SIZE,
                                  HAL_FLASH_STORE_BASE, HAL_FLASH_BANK_SIZE, HAL_FLASH_SECTOR) == NV_LOG_MIGRATION_DONE;
        if (!store_ok) app_store.mount_status = NV_LOG_ERR_CORRUPT;
    }
    crypto_setup();   /* before any key is used: selects the CCM* backend */
    const crash_record_t *crash = crash_log_previous();
    znp_boot(&znp, &ZNP_PLATFORM, &coord, &COORD_RADIO, store_ok ? &app_store : 0, ieee64(), hal_trng_u32(),
             mt_reset_reason(hal_sys_reset_source(), crash->kind == CRASH_WATCHDOG));
    for (;;) {
        uint32_t bits = 0;
        uint32_t wait = mode == MODE_COORD || mode == MODE_ZNP ? COORD_TICK_MS : TASK_CHECKIN_MS;
        if (gp_pending || gp_active || tx_fake_busy) wait = 1;   /* GP reply due within ~20 ms */
        (void)xTaskNotifyWaitIndexed(0, 0, UINT32_MAX, &bits, pdMS_TO_TICKS(wait));
        wd_quorum_checkin(&app_quorum, WD_BIT_RADIO);
        if (rf_is_powered()) {
            (void)rf_take_events();
            if (mode == MODE_COORD || mode == MODE_ZNP) poll_tx();
            drain_rx();
            if (mode == MODE_ZNP) { poll_tx(); gp_service(); }
            if (mode == MODE_COORD || mode == MODE_ZNP) {
                uint32_t t0 = hal_cycles();
                zb_coord_tick(&coord, now_ms());
                uint32_t dt = hal_cycles() - t0;
                if (dt > app_perf.tick_max) app_perf.tick_max = dt;
                snapshot();
            }
        } else {
            coord.now_ms = now_ms();   /* keeps permit-join deadlines meaningful */
        }
        if (bits & EVT_ZNP) {
            for (unsigned i = 0; i < ZNP_IN_QUEUE; i++) {
                if (xQueueReceive(app_znp_in, &znp_frame, 0) != pdPASS) break;
                static npi_frame in;
                in = znp_frame;
                uint32_t t0 = hal_cycles();
                znp_handle(&znp, in.cmd0, in.cmd1, in.payload, in.length);
                uint32_t dt = hal_cycles() - t0;
                app_perf.znp_last = dt;
                if (dt > app_perf.znp_max) app_perf.znp_max = dt;
            }
            if (uxQueueMessagesWaiting(app_znp_in)) xTaskNotifyIndexed(radio_handle, 0, EVT_ZNP, eSetBits);
            snapshot();
        }
        znp_poll(&znp);
        app_perf.radio_stack_free_words = (uint32_t)uxTaskGetStackHighWaterMark(0);
        if (bits & EVT_REQUEST) {
            uint32_t seq = slot_seq;
            slot_reply = serve(&slot_req);
            if (mode == MODE_COORD) snapshot();
            slot_done_seq = seq;
            if (slot_requester) xTaskNotifyGiveIndexed(slot_requester, 1);
        }
    }
}

bool radio_task_request(const radio_request_t *req, radio_reply_t *reply, uint32_t timeout_ms) {
    if (xSemaphoreTake(slot_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    slot_req = *req;
    uint32_t seq = ++slot_seq;
    slot_requester = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTakeIndexed(1, pdTRUE, 0);     /* discard stale gives */
    xTaskNotifyIndexed(radio_handle, 0, EVT_REQUEST, eSetBits);
    TickType_t start = xTaskGetTickCount();
    bool ok = false;
    while (xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout_ms)) {
        (void)ulTaskNotifyTakeIndexed(1, pdTRUE, pdMS_TO_TICKS(TASK_CHECKIN_MS));
        wd_quorum_checkin(&app_quorum, WD_BIT_HOST);
        if (slot_done_seq == seq) { *reply = slot_reply; ok = true; break; }
    }
    xSemaphoreGive(slot_mutex);
    return ok;
}

void radio_task_znp_notify(void) {
    if (radio_handle) xTaskNotifyIndexed(radio_handle, 0, EVT_ZNP, eSetBits);
}

void radio_task_create(void) {
    app_znp_in = xQueueCreateStatic(ZNP_IN_QUEUE, sizeof(npi_frame), znp_in_storage, &znp_in_queue);
    app_znp_out = xQueueCreateStatic(ZNP_OUT_QUEUE, sizeof(npi_frame), znp_out_storage, &znp_out_queue);
    configASSERT(app_znp_in && app_znp_out);
    slot_mutex = xSemaphoreCreateMutexStatic(&slot_mutex_buf);
    app_radio_frames = xQueueCreateStatic(RADIO_FRAME_QUEUE, sizeof(rf_rx_frame_t),
                                          frames_storage, &frames_queue);
    app_join_events = xQueueCreateStatic(JOIN_EVENT_QUEUE, sizeof(app_join_event_t),
                                         join_storage, &join_queue);
    app_aps_events = xQueueCreateStatic(APS_EVENT_QUEUE, sizeof(app_aps_event_t), aps_storage, &aps_queue);
    radio_handle = xTaskCreateStatic(radio, "radio", STACK_RADIO, 0, PRIO_RADIO, stack, &tcb);
    configASSERT(slot_mutex && app_radio_frames && app_join_events && app_aps_events && radio_handle);
}
