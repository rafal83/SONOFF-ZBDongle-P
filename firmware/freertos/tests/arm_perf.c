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

static int tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) { (void)ctx; (void)mpdu; (void)len; (void)ack; arm_perf_tx++; return 0; }
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
