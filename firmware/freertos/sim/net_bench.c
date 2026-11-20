/* SPDX-License-Identifier: MIT
 * Sustained-throughput and sleepy-device benchmark: SIMULATION, not a
 * hardware measurement.
 *
 * The REAL portable stack (transport/znp, zb_coord, MAC, NWK security) runs
 * on the PC in a discrete-event simulation (microsecond clock), driven like
 * the dongle drives it:
 *   - host: a zigbee-herdsman model (Z-Stack 3.x adapter): at most
 *     --concurrency transactions at once (the slot is held until the ZCL
 *     response, herdsman's queue), one SREQ on the UART at a time (each waits
 *     for its SRSP), AF_DATA_CONFIRM then ZCL response timeouts, herdsman's
 *     recovery (BUFFER_FULL / channel busy / no route...: 2 s pause then a new
 *     attempt, at most 4; response timeout: one more attempt);
 *   - UART at --baud (10 bits per byte), NPI framing (5 bytes + payload),
 *     ZNP queues of the firmware (4 frames each way): when the output queue
 *     is full the radio task blocks, as znp_send() does;
 *   - radio task: one CPU; per iteration, as radio_task.c: RF entries drained,
 *     TX completion, again, zb_coord_tick, then up to 8 ZNP frames. CPU time of
 *     each call = instructions of that step measured on the compiled ARM code
 *     (tools/bench_transaction_cpu.py) x 1.464 cycles per instruction, plus
 *     every CCM* operation counted during the call (native engine: 1 650
 *     cycles, average measured in production traffic; software: 6 870 cycles
 *     per AES block). 48 MHz;
 *   - RF core: --rx-entries receive buffers (4 in the firmware); a frame that
 *     finds them full is dropped without ACK; hardware ACK with the
 *     frame-pending bit from the source-match table the MAC maintains;
 *   - 802.15.4 channel at 250 kbit/s: unslotted CSMA-CA (macMinBE 3, macMaxBE
 *     5, 4 backoffs, CCA 128 us, turnaround 192 us), ACK 352 us, ACK wait
 *     864 us, any overlap of two transmissions destroys both, plus random
 *     loss --loss % per frame. Default: ONE collision domain (every node
 *     interferes with every other one: worst case for contention);
 *     --spatial-reuse: transmissions deep in two different subtrees do not
 *     interfere;
 *   - routers (tree of 1 to 3 hops): own MAC queue and CSMA, 3 retries,
 *     NWK security redone at each hop, source-routed relaying, Route Record
 *     after each many-to-one request, rebroadcast of broadcasts, ZCL Read
 *     Attributes answered after --resp-min..--resp-max ms, indirect queue
 *     for their sleepy children (--router-indirect frames, 7.68 s);
 *   - sleepy end devices: children of the coordinator or of routers, MAC
 *     data poll every --poll-ms (or all at once: --sed-sync), wait for the
 *     data announced by the frame-pending bit, answer the read, fast polls
 *     afterwards; parent loss (--kill-router): after 3 failed polls, NWK
 *     rejoin through the coordinator and Device Announce.
 *
 *   net_bench [options] --rate 100 --duration 20 [--json out.json]
 *
 * Not modelled: Flash timing (no store attached), the host task's own CPU
 * time (counted in the load, not in the radio task's timeline), PHY effects
 * other than the random loss, hidden nodes.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zb_ccm_backend.h"
#include "zb_coord.h"
#include "znp.h"

typedef uint64_t us_t;
#define MS(x) ((us_t)(x) * 1000u)
#define S(x) ((us_t)(x) * 1000000u)
#define MAX_NODES 260
#define QCAP 16
#define RXCAP 16
#define CHILD_Q 16
#define MAX_TX 24000
#define MAX_ACTIVE 256
#define PAN 0x4E5A
#define CHANNEL 15
static const uint8_t KEY[16] = {0x8A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F, 0x60, 0x71, 0x82, 0x93, 0xA4, 0xB5, 0xC6, 0xD7, 0xE8, 0xF9};
#define COORD_EXT 0x00124B00C0FFEE01ull

/* ---- configuration ------------------------------------------------------ */
static struct {
    double rate, duration_s, warmup_s, loss_pct, sed_share, reports;
    unsigned hops[3], seds_coord, seds_router, concurrency, rx_entries, znp_out_q, router_indirect;
    unsigned poll_ms, sed_sync, resp_min_ms, resp_max_ms, sed_resp_ms, fast_polls, fast_poll_ms, sed_wait_ms;
    unsigned baud, timeout_ms, sed_parent, sed_move_node, sed_move_to, sed_move_at_s, kill_router, kill_at_s, congest_router, congest_queue, spatial_reuse;
    unsigned seed, hw_aes, tick_every_wake, guessed, quiet, mesh, stall_at_ms, stall_ms, stall_count;
    /* instructions per step (tools/bench_transaction_cpu.py), CCM* excluded */
    unsigned i_af, i_confirm, i_rx, i_dup, i_poll, i_tick;
    unsigned ccm_native_cycles, host_frame_cycles;
    const char *json, *label;
} cfg = {
    .rate = 100, .duration_s = 20, .warmup_s = 5, .loss_pct = 1, .sed_share = 0,
    .hops = {16, 9, 3}, .seds_coord = 0, .seds_router = 0, .concurrency = 16, .rx_entries = 4, .znp_out_q = 4,
    .router_indirect = 8, .poll_ms = 2000, .sed_sync = 0, .resp_min_ms = 2, .resp_max_ms = 10, .sed_resp_ms = 5,
    .fast_polls = 3, .fast_poll_ms = 250, .sed_wait_ms = 50,
    .baud = 1000000, .timeout_ms = 10000, .kill_router = 0, .kill_at_s = 0, .congest_router = 0, .congest_queue = 2,
    .spatial_reuse = 0, .seed = 1, .hw_aes = 1, .tick_every_wake = 0, .guessed = 1, .quiet = 0, .mesh = 1,
    .i_af = 4395, .i_confirm = 388, .i_rx = 3516, .i_dup = 1051, .i_poll = 1040, .i_tick = 1457,
    .ccm_native_cycles = 1650, .host_frame_cycles = 1500, .json = 0, .label = "",
};
#define CPI 1.4641
#define CPU_MHZ 48.0
#ifdef ZB_ED_TIMEOUT_DEFAULT
#define AGING_STAT(c, f) ((c).stats.f)
#else
#define AGING_STAT(c, f) 0u
#endif
/* Counters that only exist in the current firmware (0 when built against an older one). */
#ifdef ZB_ANSWERED
#define ZCL_DROPPED(c) ((c).stats.zcl_answers_dropped)
#define NEW_STAT(c, f) ((c).stats.f)
#else
#define ZCL_DROPPED(c) 0u
#define NEW_STAT(c, f) 0u
#endif

/* ---- deterministic PRNG -------------------------------------------------- */
static uint64_t rng_state;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}
static unsigned rnd_range(unsigned lo, unsigned hi) { return hi <= lo ? lo : lo + rnd() % (hi - lo + 1u); }
static bool chance(double pct) { return pct > 0 && (rnd() % 1000000u) < (unsigned)(pct * 10000.0); }

/* ---- event queue (binary heap) -------------------------------------------- */
enum {
    EV_CPU_DONE, EV_CPU_TIMER, EV_CCA, EV_TX_END, EV_ACK_END, EV_ACK_WAIT, EV_NODE_PROC, EV_SED_POLL,
    EV_SED_WAIT_END, EV_HOST_GEN, EV_HOST_TIMER, EV_UART_IN_DONE, EV_UART_OUT_DONE, EV_KILL, EV_NODE_KICK,
    EV_LINK_STATUS, EV_STALL, EV_REPORT, EV_MOVE,
};
typedef struct { us_t t; uint64_t seq; uint8_t type; int a, b; uint32_t gen; } event_t;
static event_t *heap;
static size_t heap_n, heap_cap;
static uint64_t ev_seq;
static us_t now;
static void push_ev(us_t t, uint8_t type, int a, int b, uint32_t gen) {
    if (heap_n == heap_cap) {
        heap_cap = heap_cap ? heap_cap * 2 : 4096;
        heap = realloc(heap, heap_cap * sizeof *heap);
        if (!heap) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    event_t e = {t, ev_seq++, type, a, b, gen};
    size_t i = heap_n++;
    while (i) {
        size_t p = (i - 1) / 2;
        if (heap[p].t < e.t || (heap[p].t == e.t && heap[p].seq < e.seq)) break;
        heap[i] = heap[p];
        i = p;
    }
    heap[i] = e;
}
static event_t pop_ev(void) {
    event_t top = heap[0], last = heap[--heap_n];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        const event_t *best = &last;
        if (l < heap_n && (heap[l].t < best->t || (heap[l].t == best->t && heap[l].seq < best->seq))) { m = l; best = &heap[l]; }
        if (r < heap_n && (heap[r].t < best->t || (heap[r].t == best->t && heap[r].seq < best->seq))) { m = r; best = &heap[r]; }
        if (m == i) break;
        heap[i] = heap[m];
        i = m;
    }
    if (heap_n) heap[i] = last;
    return top;
}

/* ---- nodes ---------------------------------------------------------------- */
enum { K_COORD, K_ROUTER, K_SED };
typedef struct {
    uint8_t mpdu[MAC_MAX_MPDU];
    uint8_t len;
    int dst;
    bool ack, deferred;
    us_t not_before;
    nwk_header_t h;              /* deferred: NWK frame in clear, secured when sent */
    uint8_t plain[100], plain_len;
    uint16_t mac_dst;
} qframe_t;
typedef struct { uint8_t mpdu[MAC_MAX_MPDU]; uint8_t len; int child; us_t t; } child_frame_t;
typedef struct {
    int kind, parent, depth, domain;
    uint16_t short_addr;
    uint64_t ext;
    bool alive, rr_needed;
    core_security_t sec;
    uint8_t mac_seq, nwk_seq, aps_counter;
    /* MAC transmit side (routers and SEDs; the coordinator's is mac_core) */
    qframe_t q[QCAP];
    qframe_t cur;               /* frame being transmitted */
    unsigned qn, qcap;
    bool kick_armed;
    bool tx_busy;
    unsigned nb, be, retries, nwk_rounds;
    bool ack_got, ack_fp;
    /* indirect frames held for sleepy children (routers) */
    child_frame_t cq[CHILD_Q];
    unsigned cqn;
    /* duplicate filter of received MAC frames */
    uint16_t last_src[4];
    uint8_t last_seq[4];
    unsigned last_i;
    /* broadcast transaction table */
    uint32_t btt[16];
    us_t btt_t[16];
    unsigned btt_i;
    /* sleepy end device */
    unsigned poll_fails, fast_left, rejoin_state;
    bool waiting_data;
    uint32_t wait_gen, poll_gen;
    uint8_t pending_tsn[4];
    unsigned pending_n;
} node_t;
static node_t node[MAX_NODES];
static int nodes;
static int by_short(uint16_t s) {
    for (int i = 0; i < nodes; i++) if (node[i].short_addr == s) return i;
    return -1;
}

/* ---- stack under test ------------------------------------------------------ */
static zb_coord_t coord;
static znp_t znp;
static uint32_t ccm_calls;
static uint64_t ccm_cycles_acc;
static bool counting_ccm(void *ctx, bool enc, const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                         size_t aad_len, const uint8_t *in, size_t len, uint8_t *out, uint8_t mic[4]) {
    ccm_calls++;
    if (cfg.hw_aes) {
        ccm_cycles_acc += cfg.ccm_native_cycles;
    } else {   /* B0 + AAD blocks + 2 per data block + S0 */
        unsigned blocks = 1u + (unsigned)((aad_len + 2u + 15u) / 16u) + 2u * (unsigned)((len + 15u) / 16u) + 1u;
        ccm_cycles_acc += 6870ull * blocks + 600u;
    }
    return zb_ccm_backend_sw(ctx, enc, key, nonce, aad, aad_len, in, len, out, mic);
}
static bool reserve_ok(void *ctx, uint32_t u) { (void)ctx; (void)u; return true; }

/* ---- statistics --------------------------------------------------------------- */
static struct {
    uint64_t air_busy_us, cpu_busy_us, cpu_busy_window_us, cpu_peak_us, host_cycles;
    us_t cpu_window_start;
    unsigned collisions, random_losses, frames_on_air, acks_on_air;
    unsigned rx_buf_drops, rx_entries_max, coord_rx_frames, stalls;
    uint64_t stall_us;
    unsigned znp_out_max, znp_in_max, znp_blocks;
    uint64_t znp_blocked_us;
    unsigned direct_max, urgent_max, pool_max, indirect_max, track_max, disc_max, recent_max, confirms_max;
    unsigned host_backlog_max, host_inflight_max;
    unsigned router_drops, router_queue_full, router_retries, router_nwk_retries, child_q_full, child_expired;
    unsigned sed_polls, sed_fp_miss, sed_data, sed_rejoins, sed_fp_set, sed_rejoin_retries, sed_moves, assoc_removes, assoc_rpc_errors;
    unsigned dup_responses, stray_confirms, wrong_confirms, handle_collisions, response_timeouts, btt_seq_reuse;
    unsigned confirm_by_status[256];
} st;

/* ---- the air ------------------------------------------------------------------- */
typedef struct { us_t start, end; int src, domain; bool collided, is_ack; int ack_for; uint8_t len; uint8_t mpdu[MAC_MAX_MPDU]; } air_t;
static air_t air[64];
static unsigned air_used[64];   /* 1: active */
static us_t airtime(unsigned mpdu_len) { return (us_t)(mpdu_len + 2u + 6u) * 32u; }
static bool interferes(int d1, int d2) { return !cfg.spatial_reuse || d1 == 0 || d2 == 0 || d1 == d2; }
static bool channel_busy(us_t t0, us_t t1, int domain) {
    for (unsigned i = 0; i < 64; i++)
        if (air_used[i] && air[i].start < t1 && air[i].end > t0 && interferes(air[i].domain, domain)) return true;
    return false;
}
static int air_start(int src, const uint8_t *mpdu, uint8_t len, bool is_ack, us_t t) {
    unsigned slot = 64;
    for (unsigned i = 0; i < 64; i++) {
        if (air_used[i] && air[i].end + 1000 < t) air_used[i] = 0;   /* garbage */
        if (!air_used[i] && slot == 64) slot = i;
    }
    if (slot == 64) { fprintf(stderr, "air table full\n"); exit(2); }
    air_t *a = &air[slot];
    a->start = t;
    a->end = t + (is_ack ? 352u : airtime(len));
    a->src = src;
    a->domain = node[src].domain;
    a->collided = false;
    a->is_ack = is_ack;
    a->len = len;
    if (len) memcpy(a->mpdu, mpdu, len);
    for (unsigned i = 0; i < 64; i++) {
        if (i == slot || !air_used[i]) continue;
        if (air[i].start < a->end && air[i].end > a->start && interferes(air[i].domain, a->domain)) {
            if (!air[i].collided) st.collisions++;
            air[i].collided = a->collided = true;
        }
    }
    air_used[slot] = 1;
    st.air_busy_us += a->end - a->start;
    if (is_ack) st.acks_on_air++; else st.frames_on_air++;
    return (int)slot;
}

/* ---- coordinator radio (RF core model) ------------------------------------------ */
static struct {
    bool tx_pending_start;       /* MAC called tx(): CSMA starts when the CPU step ends */
    qframe_t frame;
    bool done;                   /* result waiting for poll_tx */
    mac_tx_result_t result;
    bool fp;
} crf;
typedef struct { uint8_t mpdu[MAC_MAX_MPDU]; uint8_t len; } rx_entry_t;
static rx_entry_t rx_ring[RXCAP];
static unsigned rx_h, rx_n;
static bool src_match_short[0x10000];
static uint64_t src_match_ext[8];
static void cpu_notify(void);

/* MAC radio ops */
static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx;
    memcpy(crf.frame.mpdu, mpdu, len);
    crf.frame.len = len;
    crf.frame.ack = ack;
    crf.tx_pending_start = true;
    return 0;
}
static void sim_match(void *ctx, bool ext, uint64_t a, bool p) {
    (void)ctx;
    if (!ext) { src_match_short[a & 0xFFFF] = p; return; }
    for (unsigned i = 0; i < 8; i++) if (src_match_ext[i] == a) src_match_ext[i] = 0;
    if (p) for (unsigned i = 0; i < 8; i++) if (!src_match_ext[i]) { src_match_ext[i] = a; break; }
}
static bool ext_pending(uint64_t a) { for (unsigned i = 0; i < 8; i++) if (src_match_ext[i] == a) return true; return false; }

/* ---- node transmit side --------------------------------------------------------- */
static void node_try_send(int n);
static void start_csma(int n) {
    node_t *d = &node[n];
    d->nb = 0;
    d->be = 3;
    push_ev(now + (us_t)rnd_range(0, (1u << d->be) - 1u) * 320u, EV_CCA, n, 0, 0);
}
static void node_tx_finished(int n, int result);   /* 0 ok, 1 no ack, 2 busy */

static void on_cca(int n) {
    node_t *d = &node[n];
    if (!d->alive && n != 0) { d->tx_busy = false; return; }
    if (channel_busy(now, now + 128u, d->domain)) {
        d->nb++;
        if (d->nb > 4) { node_tx_finished(n, 2); return; }
        if (d->be < 5) d->be++;
        push_ev(now + 128u + (us_t)rnd_range(0, (1u << d->be) - 1u) * 320u, EV_CCA, n, 0, 0);
        return;
    }
    const qframe_t *f = n == 0 ? &crf.frame : &d->cur;
    int slot = air_start(n, f->mpdu, f->len, false, now + 128u + 192u);
    push_ev(air[slot].end, EV_TX_END, n, slot, 0);
}

/* ---- frames helpers -------------------------------------------------------------- */
static size_t seal_nwk(node_t *d, nwk_header_t *h, const uint8_t *payload, size_t len, uint8_t *out) {
    uint8_t buf[CORE_SECURITY_MAX_FRAME];
    h->security = false;
    size_t hl = nwk_build(h, buf, sizeof buf);
    if (!hl || hl + len + 18u > sizeof buf) return 0;
    memcpy(buf + hl, payload, len);
    uint8_t local[8];
    for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(d->ext >> (8 * i));
    size_t total = 0;
    if (!core_security_seal(&d->sec, 0, local, buf, hl, len, sizeof buf, &total)) return 0;
    memcpy(out, buf, total);
    return total;
}
static uint8_t mac_wrap(node_t *d, uint16_t dst, bool ack, bool pending, const uint8_t *nwk, size_t len, uint8_t *mpdu) {
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.seq = d->mac_seq++; f.pending = pending;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = dst; f.ack_request = ack && dst != 0xFFFF;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = d->short_addr;
    return (uint8_t)mac_frame_build(&f, nwk, len, mpdu, MAC_MAX_MPDU);
}
static us_t enqueue_delay;   /* jitter of the next node_enqueue (broadcast relays, periodic frames) */
static bool node_enqueue(int n, const uint8_t *mpdu, uint8_t len, int dst, bool ack) {
    node_t *d = &node[n];
    us_t delay = enqueue_delay;
    enqueue_delay = 0;
    if (!len) return false;
    if (d->qn >= d->qcap) { st.router_queue_full++; return false; }
    qframe_t *q = &d->q[d->qn];
    memcpy(q->mpdu, mpdu, len);
    q->len = len;
    q->dst = dst;
    q->ack = ack;
    q->not_before = now + delay;
    q->deferred = false;
    d->qn++;
    node_try_send(n);
    return true;
}
/* NWK frame queued in clear, secured when transmitted. */
static bool node_enqueue_nwk(int n, int dst, uint16_t mac_dst, bool ack, const nwk_header_t *h, const uint8_t *payload,
                             size_t len) {
    node_t *d = &node[n];
    us_t delay = enqueue_delay;
    enqueue_delay = 0;
    if (len > sizeof d->q[0].plain) return false;
    if (d->qn >= d->qcap) { st.router_queue_full++; return false; }
    qframe_t *q = &d->q[d->qn];
    q->deferred = true;
    q->h = *h;
    memcpy(q->plain, payload, len);
    q->plain_len = (uint8_t)len;
    q->mac_dst = mac_dst;
    q->len = 1;
    q->dst = dst;
    q->ack = ack && mac_dst != 0xFFFF;
    q->not_before = now + delay;
    d->qn++;
    node_try_send(n);
    return true;
}
static void node_try_send(int n) {
    node_t *d = &node[n];
    if (d->tx_busy || !d->qn || !d->alive) return;
    /* first frame due (a jittered broadcast does not hold back the others) */
    unsigned pick = d->qn;
    us_t soonest = UINT64_MAX;
    for (unsigned i = 0; i < d->qn && pick == d->qn; i++) {
        if (d->q[i].not_before <= now) pick = i;
        else if (d->q[i].not_before < soonest) soonest = d->q[i].not_before;
    }
    if (pick == d->qn) {
        if (!d->kick_armed) { d->kick_armed = true; push_ev(soonest, EV_NODE_KICK, n, 0, 0); }
        return;
    }
    d->cur = d->q[pick];
    memmove(&d->q[pick], &d->q[pick + 1], (d->qn - pick - 1u) * sizeof d->q[0]);
    d->qn--;
    if (d->cur.deferred) {
        /* NWK security applied when the frame leaves (counters in transmission
         * order, as a real stack): frames sent out of queue order would
         * otherwise be refused by the receivers' replay protection. */
        uint8_t nwk[CORE_SECURITY_MAX_FRAME];
        size_t nl = seal_nwk(d, &d->cur.h, d->cur.plain, d->cur.plain_len, nwk);
        d->cur.len = nl ? mac_wrap(d, d->cur.mac_dst, d->cur.ack, false, nwk, nl, d->cur.mpdu) : 0;
        d->cur.deferred = false;
        if (!d->cur.len) { node_try_send(n); return; }
    }
    d->tx_busy = true;
    d->retries = 0;
    d->nwk_rounds = 0;
    start_csma(n);
}
/* Upstream next hop of node n (towards the coordinator). */
static int upstream(int n) { return node[n].parent; }
/* Path of relays from the coordinator side: relays[0] next to n (source route order). */
static unsigned relays_of(int n, uint16_t *relays) {
    unsigned k = 0;
    for (int p = node[n].parent; p > 0; p = node[p].parent) relays[k++] = node[p].short_addr;
    return k;
}
static void send_upstream_nwk(int from, int origin_node, nwk_header_t *h, const uint8_t *payload, size_t len) {
    (void)origin_node;
    int up = upstream(from);
    if (!node_enqueue_nwk(from, up, node[up].short_addr, true, h, payload, len)) st.router_drops++;
}
/* Route Record for `src` (a router, or a sleepy child of router `from`). */
static void send_route_record(int from, uint16_t src, int sed_child) {
    uint16_t relays[NWK_ROUTE_MAX_RELAYS];
    unsigned k = 0;
    if (sed_child >= 0) relays[k++] = node[from].short_addr;
    k += relays_of(from, relays + k);
    uint8_t rr[2 + 2 * NWK_ROUTE_MAX_RELAYS] = {0x05, (uint8_t)k};
    for (unsigned i = 0; i < k; i++) { rr[2 + 2 * i] = (uint8_t)relays[i]; rr[3 + 2 * i] = (uint8_t)(relays[i] >> 8); }
    nwk_header_t h = {0};
    h.type = NWK_TYPE_COMMAND; h.src = src; h.dst = 0x0000; h.radius = 30; h.seq = node[from].nwk_seq++;
    h.has_src_ieee = sed_child < 0;
    h.src_ieee = node[from].ext;
    send_upstream_nwk(from, from, &h, rr, 2u + 2u * k);
}

/* ---- host model (herdsman) --------------------------------------------------------- */
typedef struct {
    int target;
    us_t offered, first_send, done;
    uint8_t tsn, transid, attempts, resp_attempts, state;   /* state: 0 queued 1 sreq 2 confirm 3 response 4 wait 5 done */
    uint8_t outcome;      /* 0 pending, 1 ok, 2 failed confirm, 3 timeout */
    uint8_t last_status;
    uint32_t timer_gen;
    bool measured, early_confirm, assoc_removed;
    uint8_t util_op;   /* 1: assocRemove to send before the next attempt */
    uint8_t handle;
} txn_t;
static txn_t txn[MAX_TX];
static unsigned txn_n, host_next_q, host_inflight;
static uint8_t host_tsn, host_transid;
static int sreq_busy = -1;         /* transaction whose SREQ waits for its SRSP */
static unsigned sreq_wait[MAX_TX], sreq_h, sreq_n;
static unsigned active[MAX_ACTIVE], active_n;
static int *targets;
static unsigned ntargets, target_rr;
static us_t t_measure_start, t_measure_end;

/* UART host -> coordinator (frames serialized), coordinator -> host */
typedef struct { uint8_t cmd0, cmd1, len; uint8_t p[250]; } npi_t;
static npi_t znp_in_q[64];
static unsigned zin_h, zin_n;
static us_t uart_in_free;
static npi_t out_q[64];
static unsigned out_h, out_n;     /* frames in the firmware's ZNP output queue (incl. one on the wire) */
static bool out_wire_busy;
static npi_t pending_out[32];
static unsigned pending_out_n;   /* produced by the current CPU step */
static us_t uart_time(unsigned payload) { return (us_t)(payload + 5u) * 10u * 1000000u / cfg.baud; }

static void host_send_sreq(unsigned t);
static void host_pump(void);
static void host_on_frame(const npi_t *f);
static void cpu_kick(void);

static void znp_platform_send(void *ctx, uint8_t cmd0, uint8_t cmd1, const uint8_t *payload, uint8_t len) {
    (void)ctx;
    if (pending_out_n >= 32) { fprintf(stderr, "pending out overflow\n"); exit(2); }
    npi_t *f = &pending_out[pending_out_n++];
    f->cmd0 = cmd0; f->cmd1 = cmd1; f->len = len;
    memcpy(f->p, payload, len);
}
static void out_wire_next(void) {
    if (out_wire_busy || !out_n) return;
    out_wire_busy = true;
    push_ev(now + uart_time(out_q[out_h].len), EV_UART_OUT_DONE, 0, 0, 0);
}

/* ---- radio task (CPU) ------------------------------------------------------------------- */
enum { PH_DRAIN1, PH_TX1, PH_DRAIN2, PH_TX2, PH_TICK, PH_ZNP, PH_END };
static struct {
    bool busy, blocked, notify, idle;
    int phase;
    unsigned znp_done;
    uint32_t timer_gen;
    uint32_t last_tick_ms;
} cpu;
static uint32_t now_ms(void) { return (uint32_t)(now / 1000u) + 1u; }

static void track_queues(void) {
    unsigned pool = 0, ind = 0, track = 0, recent = 0, conf = 0;
    for (unsigned i = 0; i < MAC_POOL_SLOTS; i++) {
        pool += coord.mac.slots[i].kind != SLOT_FREE;
        ind += coord.mac.slots[i].kind == SLOT_INDIRECT_SHORT || coord.mac.slots[i].kind == SLOT_INDIRECT_EXT;
    }
    for (unsigned i = 0; i < ZB_TX_TRACK; i++) track += coord.tx_track[i].used;
    for (unsigned i = 0; i < ZB_RECENT_TX; i++) recent += coord.recent_tx[i].used;
    for (unsigned i = 0; i < ZNP_MAX_CONFIRMS; i++) conf += znp.confirms[i].used;
#define MAXI(a, b) if ((b) > (a)) (a) = (b)
    MAXI(st.pool_max, pool); MAXI(st.indirect_max, ind); MAXI(st.track_max, track); MAXI(st.recent_max, recent);
    MAXI(st.confirms_max, conf); MAXI(st.direct_max, coord.mac.direct_count); MAXI(st.urgent_max, coord.mac.urgent_count);
    MAXI(st.disc_max, coord.disc_count);
}

/* Runs one stack call; returns its CPU time. */
static us_t cost_us(unsigned instructions) {
    double cyc = instructions * CPI + (double)ccm_cycles_acc;
    ccm_cycles_acc = 0;
    return (us_t)(cyc / CPU_MHZ) + 1u;
}
static void cpu_release_outputs(void);
static void cpu_step(void);
static void cpu_begin(us_t dt) {
    cpu.busy = true;
    st.cpu_busy_us += dt;
    push_ev(now + dt, EV_CPU_DONE, 0, 0, 0);
}
/* Flash erase in progress (store compaction): the CPU fetches from the
 * Flash it is erasing and stalls; the RF core keeps receiving into its RX
 * buffers and acknowledging, nothing else runs. */
static us_t stall_until;
static void cpu_step(void) {
    if (now < stall_until) { cpu.busy = true; st.stall_us += stall_until - now; push_ev(stall_until, EV_CPU_DONE, 0, 0, 0); return; }
    for (;;) {
        switch (cpu.phase) {
        case PH_DRAIN1:
        case PH_DRAIN2:
            if (rx_n) {
                rx_entry_t e = rx_ring[rx_h];
                rx_h = (rx_h + 1u) % RXCAP;
                rx_n--;
                uint32_t polls = coord.mac.stats.data_requests, dups = coord.stats.bcast_dup_unopened;
                zb_coord_stats_t before = coord.stats;
                mac_stats_t mbefore = coord.mac.stats;
                zb_coord_rx(&coord, e.mpdu, e.len, 200, -50);
                if (getenv("NET_BENCH_TRACE")) {
                    mac_frame_t mf;
                    if (mac_frame_parse(e.mpdu, e.len, &mf) == MAC_OK && mf.type == MAC_TYPE_DATA)
                        fprintf(stderr, "%.3f coord rx from mac 0x%04X seq %u: up %d auth_fail %d dup_aps %d rr %d "
                                "mac_dup %d filtered %d malformed %d\n", now / 1e6, mf.src_short, mf.seq,
                                coord.stats.aps_data_up - before.aps_data_up,
                                coord.stats.nwk_auth_failures - before.nwk_auth_failures,
                                coord.stats.aps_duplicates - before.aps_duplicates,
                                coord.stats.route_records - before.route_records,
                                coord.mac.stats.rx_duplicates - mbefore.rx_duplicates,
                                coord.mac.stats.rx_filtered - mbefore.rx_filtered,
                                coord.stats.nwk_malformed - before.nwk_malformed);
                }
                st.coord_rx_frames++;
                unsigned instr = coord.mac.stats.data_requests != polls ? cfg.i_poll
                               : coord.stats.bcast_dup_unopened != dups ? cfg.i_dup : cfg.i_rx;
                cpu_begin(cost_us(instr));
                return;
            }
            cpu.phase++;
            break;
        case PH_TX1:
        case PH_TX2:
            cpu.phase++;
            if (crf.done) {
                crf.done = false;
                zb_coord_tx_done(&coord, crf.result, crf.fp);
                cpu_begin(cost_us(cfg.i_confirm));
                return;
            }
            break;
        case PH_TICK:
            cpu.phase++;
            if (cfg.tick_every_wake || now_ms() != cpu.last_tick_ms) {   /* radio_task.c: once per ms */
                cpu.last_tick_ms = now_ms();
                zb_coord_tick(&coord, now_ms());
                cpu_begin(cost_us(cfg.i_tick));
                return;
            }
            break;
        case PH_ZNP:
            if (zin_n && cpu.znp_done < 8) {
                npi_t f = znp_in_q[zin_h];
                zin_h = (zin_h + 1u) % 64u;
                zin_n--;
                cpu.znp_done++;
                znp_handle(&znp, f.cmd0, f.cmd1, f.p, f.len);
                st.host_cycles += cfg.host_frame_cycles;
                cpu_begin(cost_us(f.cmd0 == 0x24 && f.cmd1 == 0x01 ? cfg.i_af : 1000u));
                return;
            }
            cpu.phase++;
            break;
        default:
            if (cpu.notify) {
                cpu.notify = false;
                cpu.phase = PH_DRAIN1;
                cpu.znp_done = 0;
                break;
            }
            cpu.idle = true;
            push_ev(now + MS(10), EV_CPU_TIMER, 0, 0, ++cpu.timer_gen);
            return;
        }
    }
}
static void cpu_kick(void) {   /* a notification (RF event, ZNP frame) */
    if (cpu.busy || cpu.blocked) { cpu.notify = true; return; }
    if (cpu.idle) {
        cpu.idle = false;
        cpu.timer_gen++;
        cpu.phase = PH_DRAIN1;
        cpu.znp_done = 0;
        cpu_step();
        return;
    }
    cpu.notify = true;
}
static void cpu_notify(void) { cpu_kick(); }

/* End of a CPU step: hand the radio its frame, queue the ZNP outputs. */
static void cpu_release_outputs(void) {
    if (crf.tx_pending_start) {
        crf.tx_pending_start = false;
        node[0].tx_busy = true;
        start_csma(0);
    }
    while (pending_out_n) {
        if (out_n >= cfg.znp_out_q) {   /* znp_send() blocks the radio task */
            if (!cpu.blocked) { cpu.blocked = true; st.znp_blocks++; }
            return;
        }
        out_q[(out_h + out_n) % 64u] = pending_out[0];
        out_n++;
        MAXI(st.znp_out_max, out_n);
        memmove(pending_out, pending_out + 1, (pending_out_n - 1u) * sizeof pending_out[0]);
        pending_out_n--;
        st.host_cycles += cfg.host_frame_cycles;
        out_wire_next();
    }
    cpu.blocked = false;
}
static us_t blocked_since;
static void on_cpu_done(void) {
    cpu.busy = false;
    track_queues();
    cpu_release_outputs();
    if (cpu.blocked) { blocked_since = now; return; }
    cpu_step();
}

/* ---- coordinator: frames on air ending ------------------------------------------------- */
static void deliver_to_node(int n, const air_t *a, int from);

static bool frame_for_coord(const mac_frame_t *f) {
    if (f->dst_mode == MAC_ADDR_SHORT) return f->dst_short == 0x0000 || f->dst_short == 0xFFFF;
    if (f->dst_mode == MAC_ADDR_EXT) return f->dst_ext == COORD_EXT;
    return false;
}
/* Coordinator RF core: filtering, RX buffer, hardware ACK. */
static void coord_hear(const air_t *a) {
    mac_frame_t f;
    if (mac_frame_parse(a->mpdu, a->len, &f) != MAC_OK || !frame_for_coord(&f)) return;
    if (f.dst_mode == MAC_ADDR_SHORT && f.dst_pan != PAN && f.dst_pan != 0xFFFF) return;
    if (rx_n >= cfg.rx_entries) { st.rx_buf_drops++; return; }   /* no buffer: dropped, no ACK */
    rx_entry_t *e = &rx_ring[(rx_h + rx_n) % RXCAP];
    memcpy(e->mpdu, a->mpdu, a->len);
    e->len = a->len;
    rx_n++;
    MAXI(st.rx_entries_max, rx_n);
    if (f.ack_request) {
        bool poll = f.type == MAC_TYPE_COMMAND && f.payload_len && f.payload[0] == MAC_CMD_DATA_REQUEST;
        bool fp = poll && (f.src_mode == MAC_ADDR_SHORT ? src_match_short[f.src_short] : ext_pending(f.src_ext));
        int slot = air_start(0, 0, 0, true, a->end + 192u);
        air[slot].ack_for = a->src;
        air[slot].mpdu[0] = fp;
        air[slot].mpdu[1] = f.seq;
        push_ev(air[slot].end, EV_ACK_END, 0, slot, 0);
    }
    cpu_notify();
}

static void node_tx_finished(int n, int result) {
    node_t *d = &node[n];
    if (n == 0) {
        node[0].tx_busy = false;
        crf.done = true;
        crf.result = result == 0 ? MAC_TX_OK : result == 1 ? MAC_TX_NO_ACK : MAC_TX_BUSY;
        crf.fp = d->ack_fp;
        cpu_notify();
        return;
    }
    if (getenv("NET_BENCH_TRACE") && d->cur.len > 3)
        fprintf(stderr, "%.3f node %d tx seq %u result %d retries %u\n", now / 1e6, n, d->cur.mpdu[2], result, d->retries);
    if (result == 1 && d->retries < 3) {   /* macMaxFrameRetries */
        d->retries++;
        st.router_retries++;
        start_csma(n);
        return;
    }
    /* NWK layer retries of unicasts (Z-Stack NWK_MAX_DATA_RETRIES 2), after a short pause */
    if (result != 0 && d->cur.ack && d->kind == K_ROUTER && d->nwk_rounds < 2) {
        d->nwk_rounds++;
        d->retries = 0;
        st.router_nwk_retries++;
        push_ev(now + MS(rnd_range(2, 10)), EV_NODE_KICK, n, 1, 0);
        return;
    }
    const qframe_t *q = &d->cur;
    bool was_poll = d->kind == K_SED && q->len >= 3 && (q->mpdu[0] & 7u) == MAC_TYPE_COMMAND;
    if (result != 0) st.router_drops += !was_poll;
    d->tx_busy = false;
    if (d->kind == K_SED && was_poll) {
        void sed_poll_result(int n, int result, bool fp);
        sed_poll_result(n, result, d->ack_fp);
    }
    node_try_send(n);
}

/* Broadcast reception: the routers of adjacent depths hear each other
 * (--mesh 1, a house-sized mesh: several neighbours each); --mesh 0: tree
 * neighbours only (parent and children: worst case for broadcasts). Unicasts
 * always follow the tree. */
static bool bcast_hears(int i, int n) {
    if (node[i].parent == n || node[n].parent == i) return true;
    if (!cfg.mesh || n == 0) return false;
    int d = node[i].depth - node[n].depth;
    return d >= -1 && d <= 1 && interferes(node[i].domain, node[n].domain);
}
static void on_tx_end(int n, int slot) {
    air_t *a = &air[slot];
    bool lost = a->collided;
    if (!lost && chance(cfg.loss_pct)) { lost = true; st.random_losses++; }
    const qframe_t *q = n == 0 ? &crf.frame : &node[n].cur;
    node[n].ack_got = false;
    node[n].ack_fp = false;
    if (!lost) {
        mac_frame_t f;
        if (mac_frame_parse(a->mpdu, a->len, &f) == MAC_OK) {
            bool bcast = f.dst_mode == MAC_ADDR_SHORT && f.dst_short == 0xFFFF;
            /* the coordinator hears its one-hop neighbours and children */
            if (n != 0 && node[n].parent == 0 && (bcast || frame_for_coord(&f))) coord_hear(a);
            if (bcast) {
                for (int i = 1; i < nodes; i++)
                    if (i != n && node[i].alive && node[i].kind == K_ROUTER && bcast_hears(i, n))
                        deliver_to_node(i, a, n);
            } else if ((f.dst_mode == MAC_ADDR_SHORT && f.dst_short != 0x0000) || f.dst_mode == MAC_ADDR_EXT) {
                int r = -1;
                if (f.dst_mode == MAC_ADDR_SHORT) r = by_short(f.dst_short);
                else for (int i = 1; i < nodes && r < 0; i++) if (node[i].ext == f.dst_ext) r = i;
                if (r > 0 && node[r].alive && (node[r].parent == n || node[n].parent == r) &&
                    (node[r].kind != K_SED || node[r].waiting_data))
                    deliver_to_node(r, a, n);
            }
        }
    }
    if (!q->ack) { node_tx_finished(n, 0); return; }
    push_ev(a->end + 864u, EV_ACK_WAIT, n, 0, 0);
}
static void on_ack_end(int slot) {
    air_t *a = &air[slot];
    int to = a->ack_for;
    if (a->collided || chance(cfg.loss_pct)) return;
    node[to].ack_got = true;
    node[to].ack_fp = a->mpdu[0] != 0;
}
static void ack_from(int n, int to, bool fp, us_t end_of_frame) {
    (void)n;
    int slot = air_start(n, 0, 0, true, end_of_frame + 192u);
    air[slot].ack_for = to;
    air[slot].mpdu[0] = fp;
    push_ev(air[slot].end, EV_ACK_END, 0, slot, 0);
}

/* ---- routers and SEDs receiving ------------------------------------------------------- */
static bool dup_mac(node_t *d, const mac_frame_t *f) {
    for (unsigned i = 0; i < 4; i++)
        if (d->last_src[i] == f->src_short && d->last_seq[i] == f->seq) return true;
    d->last_src[d->last_i] = f->src_short;
    d->last_seq[d->last_i] = f->seq;
    d->last_i = (d->last_i + 1u) % 4u;
    return false;
}
/* Broadcast transaction table, entries kept nwkBroadcastDeliveryTime (Z-Stack
 * keeps them ~3 s; 9 s here, the coordinator's own value: worst case). */
#define SIM_BTT_US S(9)
static bool btt_seen(node_t *d, uint16_t src, uint8_t seq) {
    uint32_t k = (uint32_t)src << 8 | seq | 0x1000000u;
    for (unsigned i = 0; i < 16; i++)
        if (d->btt[i] == k && now - d->btt_t[i] < SIM_BTT_US) { st.btt_seq_reuse += now - d->btt_t[i] > MS(1600); return true; }
    d->btt[d->btt_i] = k;
    d->btt_t[d->btt_i] = now;
    d->btt_i = (d->btt_i + 1u) % 16u;
    return false;
}
static void router_handle(int r, const mac_frame_t *mf, int from);
static void sed_handle(int s, const mac_frame_t *mf);

static void deliver_to_node(int n, const air_t *a, int from) {
    node_t *d = &node[n];
    mac_frame_t f;
    if (mac_frame_parse(a->mpdu, a->len, &f) != MAC_OK) return;
    bool bcast = f.dst_short == 0xFFFF;
    /* ACK: routers always have a buffer; frame pending for a child's poll */
    if (!bcast && f.ack_request) {
        bool fp = false;
        if (f.type == MAC_TYPE_COMMAND && f.payload_len && f.payload[0] == MAC_CMD_DATA_REQUEST) {
            int child = by_short(f.src_short);
            for (unsigned i = 0; i < d->cqn; i++) if (d->cq[i].child == child) fp = true;
        }
        ack_from(n, from, fp, a->end);
    }
    if (!bcast && dup_mac(d, &f)) return;
    if (d->kind == K_SED) sed_handle(n, &f);
    else router_handle(n, &f, from);
}

/* A router's sleepy child polls it: deliver the oldest held frame. */
static void router_serve_poll(int r, int child) {
    node_t *d = &node[r];
    for (unsigned i = 0; i < d->cqn; i++) {
        if (d->cq[i].child != child) continue;
        if (now - d->cq[i].t > MS(7680)) continue;   /* expired, purged below */
        bool more = false;
        for (unsigned k = i + 1; k < d->cqn; k++) more |= d->cq[k].child == child;
        child_frame_t cf = d->cq[i];
        memmove(&d->cq[i], &d->cq[i + 1], (d->cqn - i - 1u) * sizeof cf);
        d->cqn--;
        if (more) cf.mpdu[0] |= 0x10u;   /* frame pending: more data */
        if (!node_enqueue(r, cf.mpdu, cf.len, child, true)) st.router_drops++;
        return;
    }
}
static void router_expire(int r) {
    node_t *d = &node[r];
    for (unsigned i = 0; i < d->cqn;) {
        if (now - d->cq[i].t > MS(7680)) {
            memmove(&d->cq[i], &d->cq[i + 1], (d->cqn - i - 1u) * sizeof d->cq[0]);
            d->cqn--;
            st.child_expired++;
        } else {
            i++;
        }
    }
}

/* ZCL Read Attributes response from node n (router or SED) for `tsn`. */
/* Unsolicited attribute report (what most of a Zigbee2MQTT network's traffic
 * is): on/off report carrying a 16-bit id, its generation time kept here. */
#define MAX_REPORTS 65536
static us_t report_t0[MAX_REPORTS];
static uint8_t report_seen[MAX_REPORTS];
static unsigned report_n, report_measured, report_ok, report_dup;
static double *report_lat;
static void send_report(int n) {
    node_t *d = &node[n];
    if (!d->alive || report_n >= MAX_REPORTS) return;
    unsigned id = report_n++;
    report_t0[id] = now;
    report_measured += now >= t_measure_start && now < t_measure_end;
    uint8_t aps[8 + 3 + 6];
    unsigned k = 0;
    aps[k++] = 0x00; aps[k++] = 0x01; aps[k++] = 0x06; aps[k++] = 0x00; aps[k++] = 0x04; aps[k++] = 0x01;
    aps[k++] = 0x01; aps[k++] = d->aps_counter++;
    aps[k++] = 0x18; aps[k++] = d->mac_seq; aps[k++] = 0x0A;                      /* report attributes */
    aps[k++] = 0x00; aps[k++] = 0x40; aps[k++] = 0x21; aps[k++] = (uint8_t)id; aps[k++] = (uint8_t)(id >> 8);   /* 0x4000 u16 = id */
    nwk_header_t h = {0};
    h.type = NWK_TYPE_DATA; h.src = d->short_addr; h.dst = 0x0000; h.radius = 30; h.seq = d->nwk_seq++;
    if (d->kind == K_ROUTER && d->rr_needed) { d->rr_needed = false; send_route_record(n, d->short_addr, -1); }
    send_upstream_nwk(n, n, &h, aps, k);
}
static void answer_read(int n, uint8_t tsn, uint8_t dst_ep, uint8_t src_ep, uint16_t cluster) {
    node_t *d = &node[n];
    uint8_t aps[8 + 3 + 4 + 12];
    unsigned k = 0;
    aps[k++] = 0x00; aps[k++] = dst_ep; aps[k++] = (uint8_t)cluster; aps[k++] = (uint8_t)(cluster >> 8);
    aps[k++] = 0x04; aps[k++] = 0x01; aps[k++] = src_ep; aps[k++] = d->aps_counter++;
    aps[k++] = 0x18; aps[k++] = tsn; aps[k++] = 0x01;                          /* read attributes response */
    aps[k++] = 0x05; aps[k++] = 0x00; aps[k++] = 0x00; aps[k++] = 0x42; aps[k++] = 10;   /* model id, string */
    memcpy(aps + k, "SIM-DEVICE", 10); k += 10;
    nwk_header_t h = {0};
    h.type = NWK_TYPE_DATA; h.src = d->short_addr; h.dst = 0x0000; h.radius = 30; h.seq = d->nwk_seq++;
    int from = n;
    if (d->kind == K_SED) from = n;   /* the SED's own frame goes to its parent */
    if (getenv("NET_BENCH_TRACE")) fprintf(stderr, "%.3f node %d answers tsn %u rr %d queue %u\n", now / 1e6, n, tsn, d->rr_needed, d->qn);
    if (d->kind == K_ROUTER && d->rr_needed) { d->rr_needed = false; send_route_record(n, d->short_addr, -1); }
    send_upstream_nwk(from, n, &h, aps, k);
}

static void router_rebroadcast(int r, const nwk_header_t *h, const uint8_t *payload, size_t len) {
    if (h->radius <= 1) return;
    nwk_header_t rh = *h;
    rh.radius--;
    enqueue_delay = MS(2) + rnd_range(0, 62000);
    (void)node_enqueue_nwk(r, -1, 0xFFFF, false, &rh, payload, len);
    enqueue_delay = MS(500) + rnd_range(0, 62000);   /* one passive-ACK retransmission */
    (void)node_enqueue_nwk(r, -1, 0xFFFF, false, &rh, payload, len);
}

static void router_handle(int r, const mac_frame_t *mf, int from) {
    node_t *d = &node[r];
    if (mf->type == MAC_TYPE_COMMAND) {
        if (mf->payload_len && mf->payload[0] == MAC_CMD_DATA_REQUEST) router_serve_poll(r, by_short(mf->src_short));
        return;
    }
    if (mf->type != MAC_TYPE_DATA) return;
    uint8_t frame[CORE_SECURITY_MAX_FRAME];
    memcpy(frame, mf->payload, mf->payload_len);
    nwk_header_t h;
    if (nwk_parse(frame, mf->payload_len, &h) != NWK_OK || !h.security) return;
    if (nwk_is_broadcast(h.dst) && btt_seen(d, h.src, h.seq)) return;
    size_t plen;
    if (!core_security_open_authenticated_peer(&d->sec, frame, mf->payload_len, h.header_len, &plen)) return;
    const uint8_t *p = frame + h.header_len + 14;
    if (nwk_is_broadcast(h.dst)) {
        if (h.type == NWK_TYPE_COMMAND && plen >= 6 && p[0] == 0x01) {
            if (p[1] & 0x18u) d->rr_needed = true;                   /* many-to-one request */
            else {
                uint16_t target = (uint16_t)(p[3] | p[4] << 8);
                int t = by_short(target);
                bool mine = target == d->short_addr || (t > 0 && node[t].kind == K_SED && node[t].parent == r);
                if (getenv("NET_BENCH_TRACE")) fprintf(stderr, "%.3f node %d got RREQ id %u for 0x%04X%s\n", now / 1e6, r, p[2], target, mine ? " (mine)" : "");
                if (mine) {   /* Route Reply towards the coordinator */
                    const uint8_t rrep[8] = {0x02, 0x00, p[2], 0x00, 0x00, (uint8_t)target, (uint8_t)(target >> 8), 3};
                    nwk_header_t rh = {0};
                    rh.type = NWK_TYPE_COMMAND; rh.src = d->short_addr; rh.dst = 0x0000; rh.radius = 30;
                    rh.seq = d->nwk_seq++; rh.has_src_ieee = true; rh.src_ieee = d->ext;
                    send_upstream_nwk(r, r, &rh, rrep, sizeof rrep);
                    if (t > 0 && node[t].kind == K_SED) send_route_record(r, target, t);
                    return;
                }
            }
        }
        if (h.type == NWK_TYPE_COMMAND && plen && p[0] == 0x08) return;   /* link status: one hop */
        router_rebroadcast(r, &h, p, plen);
        return;
    }
    (void)from;
    if (h.dst == d->short_addr) {
        if (h.type != NWK_TYPE_DATA) return;
        aps_header_t a;
        if (aps_parse(p, plen, &a) != APS_OK || a.payload_len < 3) return;
        if ((a.payload[0] & 0x03u) == 0 && a.payload[2] == 0x00) {   /* ZCL read */
            if (getenv("NET_BENCH_TRACE")) fprintf(stderr, "%.3f node %d got read tsn %u (relays %u)\n", now / 1e6, r, a.payload[1], h.relay_count);
            push_ev(now + MS(rnd_range(cfg.resp_min_ms, cfg.resp_max_ms)) + rnd_range(0, 999), EV_NODE_PROC, r,
                    a.payload[1] | a.src_endpoint << 8 | a.dst_endpoint << 16, a.cluster);
        }
        return;
    }
    /* relay */
    if (h.radius <= 1) return;
    nwk_header_t nh = h;
    nh.radius--;
    int next = -1;
    if (h.dst == 0x0000) {
        next = upstream(r);
    } else if (h.source_route) {
        if (nh.relay_index == 0) {
            next = by_short(h.dst);
        } else {
            nh.relay_index--;
            next = by_short(h.relays[nh.relay_index]);
        }
    } else {
        /* routers' own routing: down the tree towards a descendant */
        for (int t = by_short(h.dst); t > 0; t = node[t].parent)
            if (node[t].parent == r) { next = t; break; }
    }
    if (next < 0) return;
    uint8_t nwk[CORE_SECURITY_MAX_FRAME], mpdu[MAC_MAX_MPDU];
    size_t n = seal_nwk(d, &nh, p, plen, nwk);
    if (next > 0 && node[next].kind == K_SED && node[next].parent == r) {
        router_expire(r);
        if (d->cqn >= cfg.router_indirect || d->cqn >= CHILD_Q) { st.child_q_full++; return; }
        child_frame_t *cf = &d->cq[d->cqn++];
        cf->len = mac_wrap(d, node[next].short_addr, true, false, nwk, n, cf->mpdu);
        cf->child = next;
        cf->t = now;
        return;
    }
    if (next > 0 && node[next].parent != r && node[r].parent != next) return;   /* not a neighbour */
    (void)mpdu;
    if (!node_enqueue_nwk(r, next, node[next].short_addr, true, &nh, p, plen)) st.router_drops++;
}

/* ---- sleepy end devices ---------------------------------------------------------------- */
static void sed_schedule_poll(int s, us_t at) { push_ev(at, EV_SED_POLL, s, 0, ++node[s].poll_gen); }
static void sed_poll(int s) {
    node_t *d = &node[s];
    if (!d->alive || d->waiting_data) return;
    mac_frame_t f = {0};
    f.type = MAC_TYPE_COMMAND; f.ack_request = true; f.pan_id_compression = true; f.seq = d->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = node[d->parent].short_addr;
    if (d->rejoin_state) { f.src_mode = MAC_ADDR_EXT; f.src_ext = d->ext; }
    else { f.src_mode = MAC_ADDR_SHORT; f.src_short = d->short_addr; }
    const uint8_t cmd = MAC_CMD_DATA_REQUEST;
    uint8_t mpdu[32];
    uint8_t n = (uint8_t)mac_frame_build(&f, &cmd, 1, mpdu, sizeof mpdu);
    st.sed_polls++;
    if (!node_enqueue(s, mpdu, n, d->parent, true)) sed_schedule_poll(s, now + MS(cfg.poll_ms));
}
static void sed_rejoin(int s);
static void sed_announce(int s);
void sed_poll_result(int s, int result, bool fp) {
    node_t *d = &node[s];
    if (result != 0) {
        if (++d->poll_fails >= 3 && d->parent != 0) { sed_rejoin(s); return; }
        sed_schedule_poll(s, now + MS(cfg.poll_ms));
        return;
    }
    d->poll_fails = 0;
    if (fp) {
        st.sed_fp_set++;
        d->waiting_data = true;
        push_ev(now + MS(cfg.sed_wait_ms), EV_SED_WAIT_END, s, 0, ++d->wait_gen);
        return;
    }
    /* no Rejoin Response after a few polls: the device sends its request again */
    if (d->rejoin_state && ++d->rejoin_state > 5) { st.sed_rejoin_retries++; sed_rejoin(s); return; }
    if (d->fast_left) { d->fast_left--; sed_schedule_poll(s, now + MS(cfg.fast_poll_ms)); }
    else sed_schedule_poll(s, now + MS(cfg.poll_ms) + rnd_range(0, 200000));
}
static void sed_handle(int s, const mac_frame_t *mf) {
    node_t *d = &node[s];
    if (!d->waiting_data || mf->type != MAC_TYPE_DATA) return;
    d->waiting_data = false;
    d->wait_gen++;
    st.sed_data++;
    if (d->rejoin_state) { d->rejoin_state = 0; sed_announce(s); }
    uint8_t frame[CORE_SECURITY_MAX_FRAME];
    memcpy(frame, mf->payload, mf->payload_len);
    nwk_header_t h;
    size_t plen;
    bool more = mf->pending;
    if (nwk_parse(frame, mf->payload_len, &h) == NWK_OK && h.security &&
        core_security_open_authenticated_peer(&d->sec, frame, mf->payload_len, h.header_len, &plen) &&
        h.type == NWK_TYPE_DATA) {
        const uint8_t *p = frame + h.header_len + 14;
        aps_header_t a;
        if (aps_parse(p, plen, &a) == APS_OK && a.payload_len >= 3 && a.payload[2] == 0x00)
            push_ev(now + MS(cfg.sed_resp_ms), EV_NODE_PROC, s, a.payload[1] | a.src_endpoint << 8 | a.dst_endpoint << 16,
                    a.cluster);
    }
    d->fast_left = cfg.fast_polls;
    sed_schedule_poll(s, now + (more ? MS(5) : MS(cfg.fast_poll_ms)));
}
static void sed_wait_end(int s, uint32_t gen) {
    node_t *d = &node[s];
    if (gen != d->wait_gen || !d->waiting_data) return;
    d->waiting_data = false;
    st.sed_fp_miss++;     /* frame pending announced, nothing came */
    sed_schedule_poll(s, now + MS(cfg.poll_ms));
}
/* Parent lost: NWK rejoin request to the coordinator, then announce. */
static void sed_rejoin(int s) {
    node_t *d = &node[s];
    if (!d->rejoin_state) st.sed_rejoins++;
    d->poll_fails = 0;
    d->parent = 0;
    d->domain = 0;
    d->depth = 1;
    uint8_t req[2] = {0x06, 0x80};
    nwk_header_t h = {0};
    h.type = NWK_TYPE_COMMAND; h.src = d->short_addr; h.dst = 0x0000; h.radius = 1; h.seq = d->nwk_seq++;
    h.has_src_ieee = true; h.src_ieee = d->ext;
    h.has_dst_ieee = true; h.dst_ieee = COORD_EXT;
    uint8_t nwk[CORE_SECURITY_MAX_FRAME], mpdu[MAC_MAX_MPDU];
    size_t n = seal_nwk(d, &h, req, sizeof req, nwk);
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.ack_request = true; f.pan_id_compression = true; f.seq = d->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_EXT; f.src_ext = d->ext;
    uint8_t m = (uint8_t)mac_frame_build(&f, nwk, n, mpdu, sizeof mpdu);
    (void)node_enqueue(s, mpdu, m, 0, true);
    d->rejoin_state = 1;
    d->fast_left = cfg.fast_polls;
    sed_schedule_poll(s, now + MS(100));
}
/* Rejoin response received: Device Announce (broadcast to rx-on devices) via the new parent. */
static void sed_announce(int s) {
    node_t *d = &node[s];
    uint8_t nwk[CORE_SECURITY_MAX_FRAME], mpdu[MAC_MAX_MPDU];
    size_t n;
    uint8_t m;
    uint8_t ann[8 + 12] = {0x08, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, d->aps_counter++, 0x77};
    ann[9] = (uint8_t)d->short_addr; ann[10] = (uint8_t)(d->short_addr >> 8);
    for (unsigned i = 0; i < 8; i++) ann[11 + i] = (uint8_t)(d->ext >> (8 * i));
    ann[19] = 0x80;
    nwk_header_t ah = {0};
    ah.type = NWK_TYPE_DATA; ah.src = d->short_addr; ah.dst = 0xFFFD; ah.radius = 30; ah.seq = d->nwk_seq++;
    (void)nwk; (void)mpdu; (void)n; (void)m;
    (void)node_enqueue_nwk(s, 0, 0x0000, true, &ah, ann, sizeof ann);
}

/* ---- host (herdsman) ------------------------------------------------------------------------ */
static void host_queue_sreq(unsigned t) {
    sreq_wait[(sreq_h + sreq_n++) % MAX_TX] = t;
    host_pump();
}
/* One SREQ at a time on the wire; its frame enters the firmware's ZNP
 * input queue once received. */
static void host_pump(void) {
    if (sreq_busy >= 0 || !sreq_n || zin_n >= 8u) return;
    unsigned t = sreq_wait[sreq_h];
    sreq_h = (sreq_h + 1u) % MAX_TX;
    sreq_n--;
    sreq_busy = (int)t;
    host_send_sreq(t);
}
static void host_send_sreq(unsigned t) {
    txn_t *x = &txn[t];
    node_t *d = &node[x->target];
    if (x->util_op == 1) {   /* herdsman: UTIL assocRemove(ieee), then the attempt */
        npi_t u = {0x27, 0x63, 8, {0}};
        for (unsigned i = 0; i < 8; i++) u.p[i] = (uint8_t)(d->ext >> (8 * i));
        x->state = 7;
        st.assoc_removes++;
        us_t start = now > uart_in_free ? now : uart_in_free;
        uart_in_free = start + uart_time(u.len);
        znp_in_q[(zin_h + zin_n) % 64u] = u;
        push_ev(uart_in_free, EV_UART_IN_DONE, (int)t, 0, 0);
        return;
    }
    x->transid = ++host_transid;
    x->tsn = ++host_tsn;
    if (!x->first_send) x->first_send = now;
    x->attempts++;
    x->state = 1;
    npi_t f = {0x24, 0x01, 0, {0}};
    const uint8_t zcl[5] = {0x10, x->tsn, 0x00, 0x05, 0x00};
    uint8_t *p = f.p;
    p[0] = (uint8_t)d->short_addr; p[1] = (uint8_t)(d->short_addr >> 8);
    p[2] = 1; p[3] = 1; p[4] = 0x00; p[5] = 0x00; p[6] = x->transid; p[7] = 0; p[8] = 30; p[9] = 5;
    memcpy(p + 10, zcl, 5);
    f.len = 15;
    us_t start = now > uart_in_free ? now : uart_in_free;
    uart_in_free = start + uart_time(f.len);
    znp_in_q[(zin_h + zin_n) % 64u] = f;   /* reserved now, visible at UART end */
    push_ev(uart_in_free, EV_UART_IN_DONE, (int)t, 0, 0);
}
static void host_start(unsigned t) {
    if (active_n < MAX_ACTIVE) active[active_n++] = t;
    host_inflight++;
    MAXI(st.host_inflight_max, host_inflight);
    host_queue_sreq(t);
}
static void host_finish(unsigned t, uint8_t outcome) {
    txn_t *x = &txn[t];
    x->outcome = outcome;
    x->state = 5;
    x->done = now;
    x->timer_gen++;
    host_inflight--;
    for (unsigned i = 0; i < active_n; i++) if (active[i] == t) { active[i] = active[--active_n]; break; }
    if (host_next_q < txn_n) host_start(host_next_q++);
}
static void host_timer(unsigned t, uint32_t gen) {
    txn_t *x = &txn[t];
    if (gen != x->timer_gen || x->state == 5) return;
    if (x->state == 4) { host_queue_sreq(t); return; }            /* end of the 2 s pause */
    if (x->state == 2) { x->last_status = 0xFF; host_finish(t, 2); return; }   /* no confirm */
    if (x->state == 3) {                                            /* response timeout */
        st.response_timeouts++;
        if (getenv("NET_BENCH_DEBUG")) {
            uint16_t dst = node[x->target].short_addr;
            int r = -1;
            for (unsigned i = 0; i < ZB_RECENT_TX; i++) if (coord.recent_tx[i].used && coord.recent_tx[i].dst == dst) r = (int)i;
            fprintf(stderr, "%.3f response timeout node %d depth %d tsn %u, recent_tx %s retries %d\n", now / 1e6,
                    x->target, node[x->target].depth, x->tsn, r < 0 ? "none" : "kept", r < 0 ? -1 : coord.recent_tx[r].retries);
        }
        if (x->resp_attempts++ < 1) { host_queue_sreq(t); return; }
        host_finish(t, 3);
    }
}
static void host_on_frame(const npi_t *f) {
    if (sreq_busy >= 0 && txn[sreq_busy].state == 7 && ((f->cmd0 == 0x67 && f->cmd1 == 0x63) ||
                                                        (f->cmd0 == 0x60 && f->cmd1 == 0x00))) {
        txn_t *x = &txn[sreq_busy];
        if (f->cmd0 == 0x60) {   /* RPC error: herdsman's request throws, the command fails */
            st.assoc_rpc_errors++;
            x->last_status = 0xFE;
            unsigned t = (unsigned)sreq_busy;
            sreq_busy = -1;
            host_finish(t, 2);
            host_pump();
            return;
        }
        x->util_op = 0;
        host_send_sreq((unsigned)sreq_busy);   /* the attempt itself, still our SREQ turn */
        return;
    }
    if (f->cmd0 == 0x64 && f->cmd1 == 0x01) {                    /* SRSP of AF_DATA_REQUEST */
        if (sreq_busy >= 0) {
            txn_t *x = &txn[sreq_busy];
            if (x->early_confirm || x->state != 1) {   /* already confirmed (failure) or finished */
                x->early_confirm = false;
                sreq_busy = -1;
                host_pump();
                return;
            }
            x->state = 2;
            if (f->p[0] != 0) { x->last_status = f->p[0]; host_finish((unsigned)sreq_busy, 2); }
            else {
                /* the handle the firmware gave this request (for the collision check) */
                for (unsigned i = 0; i < ZNP_MAX_CONFIRMS; i++)
                    if (znp.confirms[i].used && znp.confirms[i].transid == x->transid) x->handle = znp.confirms[i].handle;
                /* another frame awaiting its MAC confirm under the same handle? */
                unsigned same = 0;
                for (unsigned i = 0; i < MAC_POOL_SLOTS; i++)
                    same += coord.mac.slots[i].kind != SLOT_FREE && coord.mac.slots[i].confirm &&
                            coord.mac.slots[i].handle == x->handle;
                for (unsigned i = 0; i < ZB_DISC_PENDING; i++)
                    same += coord.disc[i].used && coord.disc[i].handle == x->handle;
                if (same > 1) st.handle_collisions++;
                push_ev(now + MS(cfg.timeout_ms), EV_HOST_TIMER, sreq_busy, 0, ++x->timer_gen);
            }
            sreq_busy = -1;
            host_pump();
        }
        return;
    }
    if (f->cmd0 == 0x44 && f->cmd1 == 0x80) {                    /* AF_DATA_CONFIRM */
        uint8_t status = f->p[0], transid = f->p[2];
        st.confirm_by_status[status]++;
        for (unsigned i = 0; i < active_n; i++) {
            unsigned t = active[i];
            txn_t *x = &txn[t];
            if ((x->state != 1 && x->state != 2) || x->transid != transid) continue;
            if (x->state == 1) x->early_confirm = true;
            if (status == 0) {
                x->state = 3;
                push_ev(now + MS(cfg.timeout_ms), EV_HOST_TIMER, (int)t, 0, ++x->timer_gen);
                return;
            }
            x->last_status = status;
            if (getenv("NET_BENCH_DEBUG"))
                fprintf(stderr, "%.3f confirm 0x%02X for node %d (short 0x%04X depth %d) attempt %u, source route %s, "
                        "table route %s\n", now / 1e6, status, x->target, node[x->target].short_addr, node[x->target].depth,
                        x->attempts, nwk_source_route(&coord.routes, node[x->target].short_addr) ? "yes" : "no",
                        nwk_table_route(&coord.routes, node[x->target].short_addr, &(uint16_t){0}) ? "yes" : "no");
            bool recoverable = status == 0xCD || status == 0xE9 || status == 0xE1 || status == 0xF0 || status == 0x11 ||
                               status == 0x1A;
            if (!recoverable || x->attempts >= 5) { host_finish(t, 2); return; }
            if (status == 0xF0 && x->attempts == 2 && !x->assoc_removed) {
                /* herdsman: second MAC_TRANSACTION_EXPIRED -> assocGetWithAddress,
                 * assocRemove, new attempt at once (through a route discovery) */
                x->assoc_removed = true;
                x->util_op = 1;
                x->state = 4;
                x->timer_gen++;
                host_queue_sreq(t);
                return;
            }
            x->state = 4;
            push_ev(now + S(2), EV_HOST_TIMER, (int)t, 0, ++x->timer_gen);
            return;
        }
        st.stray_confirms++;
        return;
    }
    if (f->cmd0 == 0x44 && f->cmd1 == 0x81) {                    /* AF_INCOMING_MSG */
        uint16_t cluster = (uint16_t)(f->p[2] | f->p[3] << 8), src = (uint16_t)(f->p[4] | f->p[5] << 8);
        uint8_t len = f->p[16];
        const uint8_t *zcl = f->p + 17;
        if (getenv("NET_BENCH_TRACE"))
            fprintf(stderr, "%.3f incoming from 0x%04X cluster 0x%04X len %u zcl %02X %02X %02X\n", now / 1e6, src, cluster,
                    len, zcl[0], zcl[1], zcl[2]);
        if (len >= 8 && zcl[2] == 0x0A && cluster == 0x0006) {   /* one of our reports */
            unsigned id = zcl[6] | zcl[7] << 8;
            if (id < report_n) {
                if (report_seen[id]++) { report_dup++; return; }
                if (report_t0[id] >= t_measure_start && report_t0[id] < t_measure_end) {
                    if (!report_lat) report_lat = malloc(sizeof(double) * MAX_REPORTS);
                    report_lat[report_ok++] = (double)(now - report_t0[id]) / 1000.0;
                }
            }
            return;
        }
        if (len < 3 || zcl[2] != 0x01) return;
        for (unsigned i = 0; i < active_n; i++) {
            unsigned t = active[i];
            txn_t *x = &txn[t];
            if ((x->state == 2 || x->state == 3) && node[x->target].short_addr == src && x->tsn == zcl[1] &&
                cluster == 0x0000) {
                if (x->state == 2) st.wrong_confirms++;   /* answered while its confirm is still pending */
                host_finish(t, 1);
                return;
            }
        }
        st.dup_responses++;
    }
}
static void host_generate(void) {
    if (now >= t_measure_end || txn_n >= MAX_TX) return;
    txn_t *x = &txn[txn_n];
    memset(x, 0, sizeof *x);
    x->target = targets[target_rr++ % ntargets];
    if (cfg.sed_share > 0 && chance(cfg.sed_share * 100.0)) {
        int seds = cfg.seds_coord + cfg.seds_router;
        if (seds) x->target = nodes - seds + (int)(rnd() % (unsigned)seds);
    }
    x->offered = now;
    x->measured = now >= t_measure_start;
    txn_n++;
    if (host_inflight < cfg.concurrency && host_next_q == txn_n - 1) host_start(host_next_q++);
    unsigned backlog = txn_n - host_next_q;
    MAXI(st.host_backlog_max, backlog);
    double gap = 1e6 / cfg.rate;
    us_t next = now + (us_t)(gap * (0.9 + 0.2 * (rnd() % 1000u) / 1000.0));
    push_ev(next, EV_HOST_GEN, 0, 0, 0);
}

/* ---- topology -------------------------------------------------------------------------------- */
static void add_node(int kind, int parent) {
    node_t *d = &node[nodes];
    memset(d, 0, sizeof *d);
    d->kind = kind;
    d->parent = parent;
    d->depth = parent < 0 ? 0 : node[parent].depth + 1;
    d->domain = d->depth <= 1 ? 0 : (node[parent].depth == 1 ? parent : node[parent].domain);
    if (kind == K_SED && parent == 0) d->domain = 0;
    d->short_addr = nodes == 0 ? 0x0000 : (uint16_t)((kind == K_SED ? 0x8000 : 0x1000) + nodes * 7);
    d->ext = nodes == 0 ? COORD_EXT : 0x00158D0000500000ull + (uint64_t)nodes;
    d->alive = true;
    d->qcap = QCAP;
    d->mac_seq = (uint8_t)rnd();
    d->nwk_seq = (uint8_t)rnd();
    d->rr_needed = true;
    core_security_init(&d->sec, 1, zb_ccm_backend_sw, reserve_ok, 0);
    (void)core_security_set_key(&d->sec, 0, 0, KEY);
    nodes++;
}

static double pct(const double *v, unsigned n, double p) {
    if (!n) return 0;
    unsigned i = (unsigned)(p * (n - 1) + 0.5);
    return v[i];
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void link_status_round(void) {
    for (int i = 1; i < nodes; i++) {
        if (node[i].kind != K_ROUTER || node[i].depth != 1 || !node[i].alive) continue;
        const uint8_t ls[5] = {0x08, 0x61, 0x00, 0x00, 0x77};
        nwk_header_t h = {0};
        h.type = NWK_TYPE_COMMAND; h.src = node[i].short_addr; h.dst = 0xFFFC; h.radius = 1; h.seq = node[i].nwk_seq++;
        h.has_src_ieee = true; h.src_ieee = node[i].ext;
        enqueue_delay = rnd_range(0, 2000000);   /* independent periods */
        (void)node_enqueue_nwk(i, -1, 0xFFFF, false, &h, ls, sizeof ls);
    }
}

static int parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "0";
#define OPT_D(name, field) if (!strcmp(a, name)) { cfg.field = atof(v); i++; continue; }
#define OPT_U(name, field) if (!strcmp(a, name)) { cfg.field = (unsigned)strtoul(v, 0, 0); i++; continue; }
        OPT_D("--rate", rate) OPT_D("--duration", duration_s) OPT_D("--warmup", warmup_s) OPT_D("--loss", loss_pct)
        OPT_D("--sed-share", sed_share) OPT_D("--reports", reports)
        OPT_U("--hop1", hops[0]) OPT_U("--hop2", hops[1]) OPT_U("--hop3", hops[2])
        OPT_U("--seds-coord", seds_coord) OPT_U("--seds-router", seds_router) OPT_U("--concurrency", concurrency)
        OPT_U("--rx-entries", rx_entries) OPT_U("--znp-out-queue", znp_out_q) OPT_U("--router-indirect", router_indirect)
        OPT_U("--poll-ms", poll_ms) OPT_U("--sed-sync", sed_sync) OPT_U("--resp-min", resp_min_ms)
        OPT_U("--resp-max", resp_max_ms) OPT_U("--fast-polls", fast_polls) OPT_U("--sed-wait", sed_wait_ms)
        OPT_U("--baud", baud) OPT_U("--timeout", timeout_ms) OPT_U("--kill-router", kill_router)
        OPT_U("--kill-at", kill_at_s) OPT_U("--sed-parent", sed_parent) OPT_U("--sed-move-node", sed_move_node) OPT_U("--sed-move-to", sed_move_to) OPT_U("--sed-move-at", sed_move_at_s) OPT_U("--congest-router", congest_router) OPT_U("--congest-queue", congest_queue)
        OPT_U("--spatial-reuse", spatial_reuse) OPT_U("--seed", seed) OPT_U("--hw-aes", hw_aes)
        OPT_U("--tick-every-wake", tick_every_wake) OPT_U("--guessed", guessed) OPT_U("--quiet", quiet) OPT_U("--mesh", mesh) OPT_U("--stall-at", stall_at_ms) OPT_U("--stall-ms", stall_ms) OPT_U("--stall-count", stall_count)
        OPT_U("--i-af", i_af) OPT_U("--i-confirm", i_confirm) OPT_U("--i-rx", i_rx) OPT_U("--i-dup", i_dup)
        OPT_U("--i-poll", i_poll) OPT_U("--i-tick", i_tick) OPT_U("--ccm-native", ccm_native_cycles)
        if (!strcmp(a, "--json")) { cfg.json = v; i++; continue; }
        if (!strcmp(a, "--label")) { cfg.label = v; i++; continue; }
        fprintf(stderr, "unknown option %s\n", a);
        return 0;
    }
    if (cfg.rx_entries > RXCAP) cfg.rx_entries = RXCAP;
    if (cfg.znp_out_q > 64) cfg.znp_out_q = 64;
    return 1;
}

int main(int argc, char **argv) {
    if (!parse_args(argc, argv)) return 2;
    rng_state = 0x9E3779B97F4A7C15ull ^ ((uint64_t)cfg.seed * 0x100000001B3ull);
    static const mac_radio_ops_t RADIO = {sim_tx, sim_match, 0};
    static const znp_platform_t PLAT = {.send = znp_platform_send, .product = ZNP_PRODUCT_ZSTACK3X0, .revision = 20261120u};
    znp_boot(&znp, &PLAT, &coord, &RADIO, 0, COORD_EXT, 0x5EED0000u ^ cfg.seed, 0);
    coord.upper.reserve_nwk_counter = reserve_ok;
    const uint8_t epid[8] = {0xDD, 0xCC, 0xBB, 0xAA, 1, 2, 3, 4};
    zb_coord_form(&coord, PAN, CHANNEL, epid, KEY, 0);
    coord.security.ccm = counting_ccm;
    znp.formed = true;
    znp.dev_state = ZNP_DEV_ZB_COORD;
    const zb_endpoint_t ep = {.endpoint = 1, .profile = 0x0104, .device_id = 5};
    (void)zb_coord_register_endpoint(&coord, &ep);
    add_node(K_COORD, -1);
    /* routers: 1-hop, then 2-hop under the 1-hop ones, 3-hop under 2-hop */
    int first[3] = {0}, count[3];
    for (unsigned h = 0; h < 3; h++) {
        first[h] = nodes;
        count[h] = (int)cfg.hops[h];
        for (unsigned k = 0; k < cfg.hops[h]; k++) {
            int parent = h == 0 ? 0 : first[h - 1] + (int)(k % (unsigned)(count[h - 1] ? count[h - 1] : 1));
            if (h > 0 && !count[h - 1]) parent = 0;
            add_node(K_ROUTER, parent);
        }
    }
    int routers = nodes - 1;
    for (unsigned k = 0; k < cfg.seds_coord; k++) add_node(K_SED, 0);
    for (unsigned k = 0; k < cfg.seds_router; k++) add_node(K_SED, cfg.sed_parent && (int)cfg.sed_parent <= routers ? (int)cfg.sed_parent : routers ? 1 + (int)(k % (unsigned)routers) : 0);
    if (nodes > MAX_NODES) return 2;
    for (int i = 1; i < nodes; i++) {
        node_t *d = &node[i];
        bool child = d->kind == K_SED && d->parent == 0;
        if (!zb_coord_import_device(&coord, NWK_DEVICE_NONE, d->ext, d->short_addr, child, 0, 0xFF)) return 3;
        nwk_device_t *dev = &coord.devices.dev[nwk_devices_find_ext(&coord.devices, d->ext)];
        if (d->kind == K_SED && (child || !cfg.guessed)) { dev->capability = 0x80; dev->capability_guessed = false; }
        if (d->kind == K_ROUTER && !cfg.guessed) { dev->capability = 0x8E; dev->capability_guessed = false; }
    }
    if (cfg.congest_router && (int)cfg.congest_router < nodes) node[cfg.congest_router].qcap = cfg.congest_queue;
    targets = calloc((size_t)routers + 1u, sizeof *targets);
    for (int i = 1; i <= routers; i++) targets[ntargets++] = i;
    if (!ntargets) { targets[0] = nodes - 1; ntargets = 1; }

    /* warm-up: link status of the neighbours, a route record from everyone */
    cpu.idle = true;
    now = 1000;
    zb_coord_tick(&coord, now_ms());
    link_status_round();
    for (int i = 1; i < nodes; i++) {
        now = 1000 + rnd_range(0, 1500000);
        if (node[i].kind == K_ROUTER) { node[i].rr_needed = false; send_route_record(i, node[i].short_addr, -1); }
        else if (node[i].parent > 0) send_route_record(node[i].parent, node[i].short_addr, i);
        now = 1000;
    }
    for (int i = 1; i < nodes; i++) {
        if (node[i].kind != K_SED) continue;
        us_t first_poll = cfg.sed_sync ? S(cfg.warmup_s) + rnd_range(0, cfg.sed_sync * 1000u)
                                       : MS(200) + (us_t)rnd_range(0, cfg.poll_ms * 1000u);
        sed_schedule_poll(i, first_poll);
    }
    push_ev(S(15), EV_LINK_STATUS, 0, 0, 0);
    push_ev(MS(10), EV_CPU_TIMER, 0, 0, ++cpu.timer_gen);
    t_measure_start = S(cfg.warmup_s);
    t_measure_end = t_measure_start + (us_t)(cfg.duration_s * 1e6);
    if (cfg.rate > 0) push_ev(S(cfg.warmup_s) - S(1), EV_HOST_GEN, 0, 0, 0);   /* routes learnt (link status, route records) */
    if (cfg.reports > 0) push_ev(S(cfg.warmup_s) - S(1), EV_REPORT, 0, 0, 0);
    for (unsigned k = 0; k < cfg.stall_count; k++)   /* one stall per measured second from --stall-at */
        push_ev(MS(cfg.stall_at_ms) + S(k), EV_STALL, 0, 0, 0);
    if (cfg.sed_move_node && (int)cfg.sed_move_node < nodes && cfg.sed_move_to && (int)cfg.sed_move_to < nodes)
        push_ev(S(cfg.sed_move_at_s), EV_MOVE, (int)cfg.sed_move_node, (int)cfg.sed_move_to, 0);
    if (cfg.kill_router && (int)cfg.kill_router < nodes) push_ev(S(cfg.kill_at_s), EV_KILL, (int)cfg.kill_router, 0, 0);
    us_t t_stop = t_measure_end + S(40);
    us_t cpu_win = 0, cpu_win_busy0 = 0;
    while (heap_n) {
        event_t e = pop_ev();
        if (e.t > t_stop) break;
        now = e.t;
        if (now - cpu_win >= MS(100)) {
            uint64_t busy = st.cpu_busy_us - cpu_win_busy0;
            if (busy > st.cpu_peak_us && now > t_measure_start && now < t_measure_end) st.cpu_peak_us = busy;
            cpu_win = now;
            cpu_win_busy0 = st.cpu_busy_us;
        }
        switch (e.type) {
        case EV_CPU_DONE: on_cpu_done(); break;
        case EV_CPU_TIMER:
            if (e.gen == cpu.timer_gen && cpu.idle) { cpu.idle = false; cpu.phase = PH_DRAIN1; cpu.znp_done = 0; cpu_step(); }
            break;
        case EV_CCA: on_cca(e.a); break;
        case EV_TX_END: on_tx_end(e.a, e.b); break;
        case EV_ACK_END: on_ack_end(e.b); break;
        case EV_ACK_WAIT: node_tx_finished(e.a, node[e.a].ack_got ? 0 : 1); break;
        case EV_NODE_PROC: {
            node_t *d = &node[e.a];
            if (d->alive) answer_read(e.a, (uint8_t)e.b, (uint8_t)(e.b >> 8), (uint8_t)(e.b >> 16), 0x0000);
            break;
        }
        case EV_SED_POLL: if (e.gen == node[e.a].poll_gen) sed_poll(e.a); break;
        case EV_SED_WAIT_END: sed_wait_end(e.a, e.gen); break;
        case EV_HOST_GEN: host_generate(); break;
        case EV_HOST_TIMER: host_timer((unsigned)e.a, e.gen); break;
        case EV_UART_IN_DONE:
            zin_n++;
            MAXI(st.znp_in_max, zin_n);
            cpu_kick();
            break;
        case EV_UART_OUT_DONE: {
            npi_t f = out_q[out_h];
            out_h = (out_h + 1u) % 64u;
            out_n--;
            out_wire_busy = false;
            host_on_frame(&f);
            out_wire_next();
            if (cpu.blocked) {
                st.znp_blocked_us += now - blocked_since;
                cpu_release_outputs();
                if (!cpu.blocked) cpu_step();
            }
            break;
        }
        case EV_NODE_KICK:
            if (e.b) { if (node[e.a].alive && node[e.a].tx_busy) start_csma(e.a); break; }   /* NWK retry */
            node[e.a].kick_armed = false;
            node_try_send(e.a);
            break;
        case EV_MOVE: {   /* a sleepy child moves to router e.b, silently (no rejoin we hear) */
            node_t *d = &node[e.a];
            d->parent = e.b;
            d->depth = node[e.b].depth + 1;
            d->domain = node[e.b].depth == 1 ? e.b : node[e.b].domain;
            st.sed_moves++;
            break;
        }
        case EV_KILL:
            node[e.a].alive = false;
            node[e.a].qn = 0;
            node[e.a].cqn = 0;
            node[e.a].tx_busy = false;
            break;
        case EV_REPORT: {
            /* routers and sleepy devices alike, at random */
            send_report(1 + (int)(rnd() % (unsigned)(nodes - 1)));
            if (now < t_measure_end) push_ev(now + (us_t)(1e6 / cfg.reports * (0.5 + (rnd() % 1000u) / 1000.0)), EV_REPORT, 0, 0, 0);
            break;
        }
        case EV_STALL:
            stall_until = now + MS(cfg.stall_ms);
            st.stalls++;
            if (cpu.idle) cpu_kick();
            break;
        case EV_LINK_STATUS:
            link_status_round();
            for (int i = 1; i < nodes; i++) if (node[i].kind == K_ROUTER) router_expire(i);
            push_ev(now + S(15), EV_LINK_STATUS, 0, 0, 0);
            break;
        default: break;
        }
        if (now >= t_measure_end + (cfg.reports > 0 ? S(3) : 0) && !host_inflight && host_next_q == txn_n) break;
    }

    if (getenv("NET_BENCH_DEBUG")) {
        unsigned hist[6] = {0};
        for (unsigned i = 0; i < active_n; i++) hist[txn[active[i]].state < 6 ? txn[active[i]].state : 5]++;
        fprintf(stderr, "debug: now %.3f s, sreq_busy %d, sreq_n %u, zin_n %u, out_n %u, cpu busy %d blocked %d idle %d "
                        "phase %d, active %u (queued %u sreq %u confirm %u resp %u pause %u)\n",
                now / 1e6, sreq_busy, sreq_n, zin_n, out_n, cpu.busy, cpu.blocked, cpu.idle, cpu.phase, active_n, hist[0],
                hist[1], hist[2], hist[3], hist[4]);
    }
    /* ---- results ---- */
    double *lat = malloc(sizeof(double) * (txn_n + 1u)), *svc = malloc(sizeof(double) * (txn_n + 1u));
    unsigned n = 0, ok = 0, fail_confirm = 0, timeouts = 0, pending = 0, attempts = 0, ok_sed = 0, n_sed = 0;
    unsigned fail_status[256] = {0};
    for (unsigned t = 0; t < txn_n; t++) {
        const txn_t *x = &txn[t];
        if (!x->measured) continue;
        n++;
        attempts += x->attempts;
        bool sed = node[x->target].kind == K_SED;
        n_sed += sed;
        if (x->outcome == 1) {
            lat[ok] = (double)(x->done - x->offered) / 1000.0;
            svc[ok] = (double)(x->done - x->first_send) / 1000.0;
            ok++;
            ok_sed += sed;
        } else if (x->outcome == 2) { fail_confirm++; fail_status[x->last_status]++; }
        else if (x->outcome == 3) timeouts++;
        else pending++;
    }
    qsort(lat, ok, sizeof *lat, cmp_d);
    qsort(svc, ok, sizeof *svc, cmp_d);
    if (getenv("NET_BENCH_HIST")) {
        static const double edges[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 1e9};
        unsigned k = 0, cnt[9] = {0};
        for (unsigned i = 0; i < ok; i++) { while (svc[i] > edges[k]) k++; cnt[k]++; }
        fprintf(stderr, "service histogram (ms):");
        for (unsigned i = 0; i < 9; i++) fprintf(stderr, " <=%g:%u", edges[i], cnt[i]);
        fprintf(stderr, "\n");
    }
    if (report_lat) qsort(report_lat, report_ok, sizeof *report_lat, cmp_d);
    double window = cfg.duration_s;
    double cpu_load = 100.0 * (double)st.cpu_busy_us / (double)now;
    double air_load = 100.0 * (double)st.air_busy_us / (double)now;
    FILE *out = cfg.json ? fopen(cfg.json, "w") : 0;
    FILE *outs[2] = {stdout, out};
    for (unsigned o = 0; o < 2; o++) {
        FILE *fp = outs[o];
        if (!fp || (o == 0 && cfg.quiet)) continue;
        fprintf(fp, "{\"label\": \"%s\", \"simulation\": true, \"rate_offered\": %.1f, \"offered\": %u, \"ok\": %u, "
                    "\"success_pct\": %.3f, \"throughput\": %.2f, \"failed_confirm\": %u, \"timeouts\": %u, "
                    "\"unfinished\": %u, \"host_attempts\": %u,\n",
                cfg.label, cfg.rate, n, ok, n ? 100.0 * ok / n : 0, ok / window, fail_confirm, timeouts, pending, attempts);
        fprintf(fp, " \"latency_ms\": {\"p50\": %.1f, \"p95\": %.1f, \"p99\": %.1f, \"max\": %.1f}, "
                    "\"service_ms\": {\"p50\": %.1f, \"p95\": %.1f, \"p99\": %.1f},\n",
                pct(lat, ok, .5), pct(lat, ok, .95), pct(lat, ok, .99), ok ? lat[ok - 1] : 0, pct(svc, ok, .5),
                pct(svc, ok, .95), pct(svc, ok, .99));
        fprintf(fp, " \"reports\": {\"rate\": %.1f, \"sent\": %u, \"delivered\": %u, \"delivered_pct\": %.3f, "
                    "\"duplicates\": %u, \"p50_ms\": %.1f, \"p95_ms\": %.1f, \"p99_ms\": %.1f},\n",
                cfg.reports, report_measured, report_ok, report_measured ? 100.0 * report_ok / report_measured : 0,
                report_dup, report_lat ? pct(report_lat, report_ok, .5) : 0, report_lat ? pct(report_lat, report_ok, .95) : 0,
                report_lat ? pct(report_lat, report_ok, .99) : 0);
        fprintf(fp, " \"sed\": {\"transactions\": %u, \"ok\": %u, \"polls\": %u, \"fp_set\": %u, \"fp_miss\": %u, "
                    "\"data\": %u, \"rejoins\": %u, \"rejoin_retries\": %u, \"router_child_queue_full\": %u, \"router_child_expired\": %u, "
                    "\"coord_indirect_expired\": %u, \"coord_poll_queue_full\": %u, \"moves\": %u, \"assoc_removes\": %u, "
                    "\"assoc_rpc_errors\": %u, \"children_moved\": %u, \"children_aged\": %u},\n",
                n_sed, ok_sed, st.sed_polls, st.sed_fp_set, st.sed_fp_miss, st.sed_data, st.sed_rejoins, st.sed_rejoin_retries, st.child_q_full,
                st.child_expired, coord.mac.stats.indirect_expired, coord.mac.stats.queue_full, st.sed_moves, st.assoc_removes,
                st.assoc_rpc_errors, AGING_STAT(coord, children_moved), AGING_STAT(coord, children_aged));
        fprintf(fp, " \"retransmissions\": {\"mac_retries\": %u, \"mac_rounds\": %u, \"zcl_retx\": %u, "
                    "\"router_mac_retries\": %u, \"router_nwk_retries\": %u, \"host_retries\": %u},\n",
                coord.mac.stats.tx_retries, coord.mac.stats.tx_rounds, coord.stats.net_status_retries, st.router_retries,
                st.router_nwk_retries,
                attempts > n ? attempts - n : 0);
        fprintf(fp, " \"radio\": {\"air_load_pct\": %.1f, \"frames\": %u, \"acks\": %u, \"collisions\": %u, "
                    "\"random_losses\": %u, \"coord_cca_fail\": %u, \"coord_no_ack\": %u, \"rx_buffer_drops\": %u, "
                    "\"rx_entries_max\": %u, \"router_drops\": %u, \"router_queue_full\": %u, \"rreq\": %u, \"mtorr\": %u, "
                    "\"flash_stalls\": %u, \"flash_stall_ms\": %.1f},\n",
                air_load, st.frames_on_air, st.acks_on_air, st.collisions, st.random_losses, coord.mac.stats.tx_busy,
                coord.mac.stats.tx_no_ack, st.rx_buf_drops, st.rx_entries_max, st.router_drops, st.router_queue_full,
                coord.stats.rreq_tx, coord.stats.mtorr_tx, st.stalls, st.stall_us / 1000.0);
        fprintf(fp, " \"queues\": {\"mac_direct_max\": %u, \"mac_direct_cap\": %u, \"mac_urgent_max\": %u, "
                    "\"mac_pool_max\": %u, \"mac_pool_cap\": %u, \"mac_indirect_max\": %u, \"tx_track_max\": %u, "
                    "\"disc_max\": %u, \"recent_tx_max\": %u, \"znp_confirms_max\": %u, \"znp_in_max\": %u, "
                    "\"znp_out_max\": %u, \"znp_out_blocks\": %u, \"znp_out_blocked_ms\": %.1f, \"mac_refused\": %u, "
                    "\"host_inflight_max\": %u, \"host_backlog_max\": %u},\n",
                st.direct_max, MAC_DIRECT_QUEUE, st.urgent_max, st.pool_max, MAC_POOL_SLOTS, st.indirect_max,
                st.track_max, st.disc_max, st.recent_max, st.confirms_max, st.znp_in_max, st.znp_out_max, st.znp_blocks,
                st.znp_blocked_us / 1000.0, coord.mac.stats.queue_full + coord.mac.stats.pool_full, st.host_inflight_max,
                st.host_backlog_max);
        fprintf(fp, " \"cpu\": {\"radio_task_load_pct\": %.2f, \"peak_100ms_pct\": %.1f, \"host_task_load_pct\": %.2f, "
                    "\"ccm_ops\": %u, \"aes\": \"%s\"},\n",
                cpu_load, st.cpu_peak_us / 1000.0, 100.0 * (double)st.host_cycles / CPU_MHZ / (double)now, ccm_calls,
                cfg.hw_aes ? "native" : "software");
        fprintf(fp, " \"memory\": {\"zb_coord_t\": %u, \"znp_t\": %u, \"mac_slot\": %u},\n", (unsigned)sizeof(zb_coord_t),
                (unsigned)sizeof(znp_t), (unsigned)sizeof(mac_slot_t));
        fprintf(fp, " \"correctness\": {\"duplicate_responses\": %u, \"stray_confirms\": %u, \"response_before_confirm\": %u, "
                    "\"handle_collisions\": %u, \"nwk_auth_failures\": %u, \"aps_duplicates\": %u, "
                    "\"zcl_answers_dropped\": %u, \"bcast_seq_reused_dropped\": %u},\n",
                st.dup_responses, st.stray_confirms, st.wrong_confirms, st.handle_collisions, coord.stats.nwk_auth_failures,
                coord.stats.aps_duplicates, ZCL_DROPPED(coord), st.btt_seq_reuse);
        fprintf(fp, " \"routing\": {\"no_route\": %u, \"disc_buffered\": %u, \"disc_sent\": %u, \"disc_expired\": %u, "
                    "\"rreq_coalesced\": %u, \"rreq_suppressed\": %u, \"rrep_rx\": %u, \"route_records\": %u, "
                    "\"network_status_rx\": %u, \"route_failures\": %u, \"neighbors_dropped\": %u, \"early_mtorr\": %u, "
                    "\"tx_not_link_failures\": %u, \"zcl_route_rebuilds\": %u, \"bcast_seq_skipped\": %u, "
                    "\"poll_children\": %u, \"zcl_retx_sleepy_skipped\": %u},\n",
                coord.stats.no_route, coord.stats.disc_buffered, coord.stats.disc_sent, coord.stats.disc_expired,
                coord.stats.rreq_coalesced, coord.stats.rreq_suppressed, coord.stats.rrep_rx, coord.stats.route_records,
                coord.stats.network_status_rx, coord.stats.route_failures, coord.stats.neighbors_dropped,
                coord.stats.early_mtorr, coord.stats.tx_not_link_failures, NEW_STAT(coord, zcl_route_rebuilds),
                NEW_STAT(coord, bcast_seq_skipped), NEW_STAT(coord, poll_children), NEW_STAT(coord, zcl_retx_sleepy_skipped));
        fprintf(fp, " \"failures_by_status\": {");
        bool first_s = true;
        for (unsigned s = 0; s < 256; s++)
            if (fail_status[s]) { fprintf(fp, "%s\"0x%02X\": %u", first_s ? "" : ", ", s, fail_status[s]); first_s = false; }
        fprintf(fp, "},\n \"topology\": {\"routers\": [%u, %u, %u], \"seds_coord\": %u, \"seds_router\": %u, "
                    "\"spatial_reuse\": %u, \"loss_pct\": %.1f, \"baud\": %u, \"concurrency\": %u}}\n",
                cfg.hops[0], cfg.hops[1], cfg.hops[2], cfg.seds_coord, cfg.seds_router, cfg.spatial_reuse, cfg.loss_pct,
                cfg.baud, cfg.concurrency);
    }
    if (out) fclose(out);
    free(lat);
    free(svc);
    return 0;
}
