/* SPDX-License-Identifier: MIT
 * Test-only end-to-end scenarios for the Unicorn ARM run (instruction counts
 * of the real compiled coordinator / ZNP paths). Linked only into the
 * build/freertos/arm-test ELF, never into the firmware.
 */
#include "aes128.h"
#include "zb_ccm_backend.h"
#include "zb_coord.h"
#include "znp.h"

static zb_coord_t coord;
static znp_t znp;
static core_security_t dev_sec;
static uint8_t dev_seq, dev_aps;
uint8_t arm_perf_frame[MAC_MAX_MPDU];
uint32_t arm_perf_frame_len, arm_perf_tx, arm_perf_znp_out, arm_perf_aps_up;

static const uint8_t KEY[16] = {0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C, 0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39};
#define DEV_EXT 0xA4C1380000000077ull
#define DEV_SHORT 0x4E21u

uint8_t arm_perf_last_tx[MAC_MAX_MPDU];
uint32_t arm_perf_last_tx_len;
static bool arm_perf_capture;
static int tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx; (void)ack;
    if (arm_perf_capture) {
        for (unsigned i = 0; i < len && i < sizeof arm_perf_last_tx; i++) arm_perf_last_tx[i] = mpdu[i];
        arm_perf_last_tx_len = len;
    }
    arm_perf_tx++;
    return 0;
}
static void match(void *ctx, bool e, uint64_t a, bool p) { (void)ctx; (void)e; (void)a; (void)p; }
static bool reserve(void *ctx, uint32_t u) { (void)ctx; (void)u; return true; }
static bool rnd(void *ctx, uint8_t k[16]) { (void)ctx; for (unsigned i = 0; i < 16; i++) k[i] = (uint8_t)(i * 13 + 1); return true; }
static void znp_out(void *ctx, uint8_t c0, uint8_t c1, const uint8_t *p, uint8_t n) { (void)ctx; (void)c0; (void)c1; (void)p; (void)n; arm_perf_znp_out++; }
static void aps_up(void *ctx, const nwk_header_t *n, const aps_header_t *a, const uint8_t *p, size_t l, uint8_t q) {
    (void)ctx; (void)n; (void)a; (void)p; (void)l; (void)q; arm_perf_aps_up++;
}

static void (*znp_aps_data)(void *, const nwk_header_t *, const aps_header_t *, const uint8_t *, size_t, uint8_t);

void arm_perf_setup(void) {
    static const mac_radio_ops_t radio = {tx, match, 0};
    static const znp_platform_t plat = {.send = znp_out, .random = rnd, .product = 1, .revision = 20261118u};
    znp_boot(&znp, &plat, &coord, &radio, 0, 0x00124B0001020304ull, 0x1234u, 0);
    coord.upper.reserve_nwk_counter = reserve;   /* no store in this run */
    znp_aps_data = coord.upper.aps_data;
    coord.upper.aps_data = aps_up;               /* measure the coordinator, not the ZNP formatting */
    const uint8_t epid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    zb_coord_form(&coord, 0x1A62, 15, epid, KEY, 0);
    zb_coord_tick(&coord, 1);
    znp.formed = true;
    znp.dev_state = ZNP_DEV_ZB_COORD;
    const zb_endpoint_t ep = {.endpoint = 1, .profile = 0x0104, .device_id = 5};
    (void)zb_coord_register_endpoint(&coord, &ep);
    (void)zb_coord_import_device(&coord, NWK_DEVICE_NONE, DEV_EXT, DEV_SHORT, true, 0, 0xFF);
    core_security_init(&dev_sec, 1, zb_ccm_backend_sw, reserve, 0);
    (void)core_security_set_key(&dev_sec, 0, 0, KEY);
}

/* The device secures a ZCL attribute report for us (not measured). */
uint32_t arm_perf_build_rx(void) {
    nwk_header_t h = {0};
    h.type = NWK_TYPE_DATA; h.dst = 0x0000; h.src = DEV_SHORT; h.radius = 30; h.seq = dev_seq++;
    uint8_t nwk[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(&h, nwk, sizeof nwk);
    const uint8_t aps[8 + 12] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, dev_aps++,
                                 0x18, 0x22, 0x0A, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0x20, 0x7F, 0x00};
    for (unsigned i = 0; i < sizeof aps; i++) nwk[hl + i] = aps[i];
    uint8_t local[8];
    for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(DEV_EXT >> (8 * i));
    size_t total = 0;
    if (!core_security_seal(&dev_sec, 0, local, nwk, hl, sizeof aps, sizeof nwk, &total)) return 0;
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.ack_request = true; f.seq = dev_seq;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = 0x1A62; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = DEV_SHORT;
    arm_perf_frame_len = (uint32_t)mac_frame_build(&f, nwk, total, arm_perf_frame, sizeof arm_perf_frame);
    return arm_perf_frame_len;
}

/* Measured: receive path of that frame (MAC, NWK security, APS, upper). */
uint32_t arm_perf_rx(void) {
    zb_coord_rx(&coord, arm_perf_frame, (uint8_t)arm_perf_frame_len, 200, -50);
    return arm_perf_aps_up;
}

/* Measured (stack depth): the same frame up to the ZNP AF_INCOMING_MSG. */
uint32_t arm_perf_rx_znp(void) {
    coord.upper.aps_data = znp_aps_data;
    zb_coord_rx(&coord, arm_perf_frame, (uint8_t)arm_perf_frame_len, 200, -50);
    coord.upper.aps_data = aps_up;
    return arm_perf_znp_out;
}

/* Measured: one ZCL command to the device (APS + NWK security + MAC queue). */
uint32_t arm_perf_send(void) {
    const uint8_t zcl[3] = {0x01, 0x42, 0x02};
    return zb_coord_send_aps(&coord, DEV_SHORT, 1, 1, 6, 0x0104, zcl, 3, false);
}

/* Measured: the same through ZNP (AF_DATA_REQUEST parsing, SRSP). */
uint32_t arm_perf_znp_af(void) {
    const uint8_t req[10 + 3] = {(uint8_t)DEV_SHORT, (uint8_t)(DEV_SHORT >> 8), 1, 1, 0x06, 0x00, 0x33, 0x00, 30, 3,
                                 0x01, 0x43, 0x02};
    znp_handle(&znp, 0x24, 0x01, req, sizeof req);
    return arm_perf_znp_out;
}

/* Not measured: completes the transmission in flight. */
void arm_perf_tx_done(void) { zb_coord_tx_done(&coord, MAC_TX_OK, false); }

/* Measured: CCM* seal of a 40-byte payload with 22 bytes of AAD (software). */
uint32_t arm_perf_ccm_sw(void) {
    uint8_t nonce[13] = {0}, aad[22] = {0}, in[40] = {0}, out[40], mic[4];
    return zb_ccm_backend_sw(0, true, KEY, nonce, aad, sizeof aad, in, sizeof in, out, mic);
}

/* Measured: the hardware CCM* backend (block hook = software AES here),
 * in place, 100-byte payload: copy, first-block check, audit buffers. */
static bool sw_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    aes128_ctx_t a;
    aes128_init(&a, key);
    aes128_encrypt(&a, in, out);
    aes128_clear(&a);
    return true;
}
uint32_t arm_perf_ccm_hw_inplace(void) {
    static uint8_t buf[100];
    uint8_t nonce[13] = {0}, aad[22] = {0}, mic[4];
    zb_ccm_backend_set_block(sw_block);
    bool ok = zb_ccm_backend_hw(0, true, KEY, nonce, aad, sizeof aad, buf, sizeof buf, buf, mic) &&
              zb_ccm_backend_hw(0, false, KEY, nonce, aad, sizeof aad, buf, sizeof buf, buf, mic);
    zb_ccm_backend_set_block(0);
    return ok;
}

/* Measured (stack depth): periodic duties incl. a Link Status round. */
uint32_t arm_perf_tick_link_status(void) {
    coord.next_link_status_ms = 0;
    zb_coord_tick(&coord, coord.now_ms + 20000u);
    return coord.stats.link_status_tx;
}

/* Measured (stack depth): Mgmt_Lqi_req served by the coordinator. */
uint32_t arm_perf_zdo_mgmt_lqi(void) {
    static uint8_t out[128];
    const uint8_t req[2] = {0x42, 0x00};
    return (uint32_t)zb_coord_zdo_local(&coord, 0x0031, req, sizeof req, false, out, sizeof out);
}

/* ---- Workloads of the published ZeroStack benchmarks (docs/firmware/benchmarks.md),
 * reproduced on our code. Same harness: instruction counts, no peripherals. ---- */
#include "nv_log.h"

/* Fill the device table to its capacity (200) with distinct IEEE/short. */
uint32_t arm_perf_fill_devices(void) {
    uint32_t x = 0x9E3779B9u;
    for (unsigned i = 0; coord.devices.count < NWK_MAX_DEVICES && i < 4 * NWK_MAX_DEVICES; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        uint64_t ext = 0x00124B0000000000ull | ((uint64_t)x << 8) | (i & 0xFFu);
        uint16_t sa = (uint16_t)(0x2000u + i * 37u);   /* clear of the routers 0x0101.. */
        (void)zb_coord_import_device(&coord, NWK_DEVICE_NONE, ext, sa, false, 0, 0xFF);
    }
    return coord.devices.count;
}

/* Lookup of a device that is not in the (full) table: what a join or a
 * frame from an unknown device costs before any key work. */
uint32_t arm_perf_lookup_miss(uint32_t salt) {
    return nwk_devices_find_ext(&coord.devices, 0x0017880100000000ull | salt);
}

/* The coordinator broadcasts a ZCL frame; returns the MPDU length captured. */
uint32_t arm_perf_broadcast(void) {
    const uint8_t zcl[3] = {0x01, 0x44, 0x00};
    arm_perf_last_tx_len = 0;
    arm_perf_capture = true;
    (void)zb_coord_send_aps_ex(&coord, 0xFFFDu, false, 0xFF, 1, 6, 0x0104, zcl, 3, false);
    for (unsigned i = 0; i < 20 && !arm_perf_last_tx_len; i++) {
        zb_coord_tick(&coord, coord.now_ms + 10u);
    }
    arm_perf_capture = false;
    return arm_perf_last_tx_len;
}

/* Routers 1..n known to the coordinator (device table), as in a mesh. */
static core_security_t rt_sec;
#define ROUTER_EXT(r) (0x00158D0000AA0000ull | (r))
#define ROUTER_SHORT(r) ((uint16_t)(0x0100u + (r)))
uint32_t arm_perf_add_routers(uint32_t n) {
    core_security_init(&rt_sec, 1, zb_ccm_backend_sw, reserve, 0);
    (void)core_security_set_key(&rt_sec, 0, 0, KEY);
    uint32_t ok = 0;
    for (uint32_t r = 1; r <= n; r++)
        ok += zb_coord_import_device(&coord, NWK_DEVICE_NONE, ROUTER_EXT(r), ROUTER_SHORT(r), false, 0, 0xFF);
    return ok;
}

/* Measured: router `r` relays the coordinator's last broadcast, as a real
 * Zigbee PRO router does: NWK header kept (source 0x0000, same sequence),
 * radius - 1, NWK security redone hop by hop with ITS IEEE and counter,
 * MAC broadcast from its short address. The re-securing is not measured
 * (built by build_echo), only the coordinator's reception. */
static uint8_t echo[MAC_MAX_MPDU];
static uint32_t echo_len;
static uint8_t bc_plain[CORE_SECURITY_MAX_FRAME], echo_seq;
static size_t bc_hl, bc_plen;
/* Router 1 decrypts the coordinator's broadcast once; every router then
 * re-secures the same plaintext with its own identity. Returns the MPDU
 * length, or 1..4 (failed step). */
uint32_t arm_perf_build_echo(uint32_t r) {
    if (r == 1) {
        mac_frame_t f;
        if (mac_frame_parse(arm_perf_last_tx, arm_perf_last_tx_len, &f) != MAC_OK) return 1;
        uint8_t nwk[CORE_SECURITY_MAX_FRAME];
        for (unsigned i = 0; i < f.payload_len; i++) nwk[i] = f.payload[i];
        nwk_header_t h;
        if (nwk_parse(nwk, f.payload_len, &h) != NWK_OK) return 2;
        bc_hl = h.header_len;
        if (!core_security_open_authenticated_peer(&dev_sec, nwk, f.payload_len, bc_hl, &bc_plen)) return 3;
        for (size_t i = 0; i < bc_hl; i++) bc_plain[i] = nwk[i];
        bc_plain[1] &= (uint8_t)~0x02u;                     /* re-secured below */
        bc_plain[6] = (uint8_t)(bc_plain[6] - 1u);          /* radius - 1 */
        for (size_t i = 0; i < bc_plen; i++) bc_plain[bc_hl + i] = nwk[bc_hl + 14u + i];
    }
    uint8_t plain[CORE_SECURITY_MAX_FRAME];
    for (size_t i = 0; i < bc_hl + bc_plen; i++) plain[i] = bc_plain[i];
    uint8_t local[8];
    for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(ROUTER_EXT(r) >> (8 * i));
    size_t total = 0;
    if (!core_security_seal(&rt_sec, 0, local, plain, bc_hl, bc_plen, sizeof plain, &total)) return 4;
    mac_frame_t m = {0};
    m.type = MAC_TYPE_DATA; m.pan_id_compression = true; m.seq = echo_seq++;
    m.dst_mode = MAC_ADDR_SHORT; m.dst_pan = 0x1A62; m.dst_short = 0xFFFF;
    m.src_mode = MAC_ADDR_SHORT; m.src_short = ROUTER_SHORT(r);
    echo_len = (uint32_t)mac_frame_build(&m, plain, total, echo, sizeof echo);
    return echo_len;
}
uint32_t arm_perf_echo(void) {
    uint32_t dup = coord.stats.broadcast_duplicates, auth = coord.stats.nwk_auth_failures;
    uint32_t mal = coord.stats.nwk_malformed, up = arm_perf_aps_up;
    zb_coord_rx(&coord, echo, (uint8_t)echo_len, 200, -50);
    /* bits: 0 duplicate, 1 authentication failure, 2 malformed, 3 delivered up */
    return (coord.stats.broadcast_duplicates != dup) | (coord.stats.nwk_auth_failures != auth) << 1 |
           (coord.stats.nwk_malformed != mal) << 2 | (arm_perf_aps_up != up) << 3;
}

/* Measured: the last accepted frame replayed (new MAC sequence, same NWK
 * frame counter). Returns 1 when it was NOT delivered upwards. */
uint32_t arm_perf_replay(void) {
    uint32_t up = arm_perf_aps_up;
    arm_perf_frame[2] ^= 0x5Au;
    zb_coord_rx(&coord, arm_perf_frame, (uint8_t)arm_perf_frame_len, 200, -50);
    return arm_perf_aps_up == up;
}

/* Measured: fresh frame whose MIC was altered (decrypted, then rejected). */
uint32_t arm_perf_tamper(void) {
    uint32_t up = arm_perf_aps_up, fail = coord.stats.nwk_auth_failures;
    arm_perf_frame[arm_perf_frame_len - 1] ^= 0x01u;
    zb_coord_rx(&coord, arm_perf_frame, (uint8_t)arm_perf_frame_len, 200, -50);
    return arm_perf_aps_up == up && coord.stats.nwk_auth_failures == fail + 1;
}

/* Measured: fresh frame truncated inside the auxiliary security header. */
uint32_t arm_perf_truncated(uint32_t keep) {
    uint32_t up = arm_perf_aps_up;
    zb_coord_rx(&coord, arm_perf_frame, (uint8_t)keep, 200, -50);
    return arm_perf_aps_up == up;
}

/* ---- nv_log on a RAM model of the production store geometry
 * (storage layout 2: 2 banks x 56 KiB, 8 KiB sectors, NOR programming = AND). ---- */
#define BENCH_SECTOR 0x2000u
#define BENCH_BANK 0xE000u
static uint8_t bench_nor[2u * BENCH_BANK];
static nv_log_t bench_log;
uint32_t arm_nv_programmed, arm_nv_erases;
static bool nv_r(void *c, uint32_t a, void *b, size_t n) {
    (void)c; uint8_t *o = b;
    for (size_t i = 0; i < n; i++) o[i] = bench_nor[a + i];
    return true;
}
static bool nv_p(void *c, uint32_t a, const void *d, size_t n) {
    (void)c; const uint8_t *s = d;
    for (size_t i = 0; i < n; i++) bench_nor[a + i] &= s[i];
    arm_nv_programmed += (uint32_t)n;
    return true;
}
static bool nv_e(void *c, uint32_t a) {
    (void)c;
    for (size_t i = 0; i < BENCH_SECTOR; i++) bench_nor[a + i] = 0xFF;
    arm_nv_erases++;
    return true;
}
uint32_t arm_nv_setup(void) {
    static const nv_flash_ops_t ops = {nv_r, nv_p, nv_e, 0};
    for (size_t i = 0; i < sizeof bench_nor; i++) bench_nor[i] = 0xFF;
    arm_nv_programmed = arm_nv_erases = 0;
    return nv_log_mount(&bench_log, &ops, 0, BENCH_BANK, BENCH_SECTOR);
}
/* Write `len` bytes of a pattern derived from `tag` to record `id`. */
uint32_t arm_nv_write(uint32_t id, uint32_t len, uint32_t tag) {
    static uint8_t buf[NV_LOG_MAX_RECORD];
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(tag * 31u + i);
    return nv_log_write(&bench_log, (uint16_t)id, buf, (uint16_t)len);
}
uint32_t arm_nv_compactions(void) { return bench_log.stats.compactions; }

