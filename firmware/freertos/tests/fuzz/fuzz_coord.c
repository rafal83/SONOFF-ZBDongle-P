/* SPDX-License-Identifier: MIT
 * libFuzzer: the whole coordinator (zb_coord + ZNP) with a formed network,
 * driven by the input. First byte = what the input is:
 *   0: a raw MPDU heard on the air (unauthenticated paths);
 *   1: a NWK frame (header + payload) secured with the REAL network key by
 *      the harness, so that everything behind decryption is reached (NWK
 *      commands, APS, ZDO, Trust Center, relaying, Update Device...);
 *   2: a ZNP request from the host (cmd0, cmd1, payload);
 *   3: time passes (ticks, MAC completions).
 * State persists between inputs, as on the device. Nothing is stored.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zb_ccm_backend.h"
#include "zb_coord.h"
#include "znp.h"

static zb_coord_t coord;
static znp_t znp;
static core_security_t dev_sec;
static bool ready;
static uint32_t now_ms = 1;
static uint8_t mac_seq;
static const uint8_t KEY[16] = {0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C, 0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39};
#define DEV_EXT 0xA4C1380000000077ull
#define DEV_SHORT 0x4E21u
#define PAN 0x1A62u

static int tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) { (void)ctx; (void)mpdu; (void)len; (void)ack; return 0; }
static void match(void *ctx, bool e, uint64_t a, bool p) { (void)ctx; (void)e; (void)a; (void)p; }
static bool reserve(void *ctx, uint32_t u) { (void)ctx; (void)u; return true; }
static bool rnd(void *ctx, uint8_t k[16]) { (void)ctx; for (unsigned i = 0; i < 16; i++) k[i] = (uint8_t)(i * 13 + 1); return true; }
static void znp_out(void *ctx, uint8_t c0, uint8_t c1, const uint8_t *p, uint8_t n) { (void)ctx; (void)c0; (void)c1; (void)p; (void)n; }
static void reset(void *ctx) { (void)ctx; }
static void network_up(void *ctx, uint8_t ch) { (void)ctx; (void)ch; }
static int8_t power(void *ctx, int8_t dbm) { (void)ctx; return dbm; }

static void setup(void) {
    static const mac_radio_ops_t radio = {tx, match, 0};
    static const znp_platform_t plat = {.send = znp_out, .reset = reset, .random = rnd, .network_up = network_up,
                                        .set_tx_power = power, .revision = 20261118u, .product = 1};
    znp_boot(&znp, &plat, &coord, &radio, 0, 0x00124B0001020304ull, 0x1234u, 0);
    coord.upper.reserve_nwk_counter = reserve;
    const uint8_t epid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    zb_coord_form(&coord, PAN, 15, epid, KEY, 0);
    zb_coord_tick(&coord, now_ms);
    znp.formed = true;
    znp.dev_state = ZNP_DEV_ZB_COORD;
    const zb_endpoint_t ep = {.endpoint = 1, .profile = 0x0104, .device_id = 5};
    (void)zb_coord_register_endpoint(&coord, &ep);
    (void)zb_coord_import_device(&coord, NWK_DEVICE_NONE, DEV_EXT, DEV_SHORT, true, 0, 0xFF);
    core_security_init(&dev_sec, 1, zb_ccm_backend_sw, reserve, 0);
    (void)core_security_set_key(&dev_sec, 0, 0, KEY);
    ready = true;
}

static void air(const uint8_t *nwk, size_t len, uint16_t src) {
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.ack_request = true; f.seq = mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = src;
    uint8_t mpdu[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&f, nwk, len, mpdu, sizeof mpdu);
    if (n) zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
}

static void drain(void) {
    for (unsigned i = 0; i < 64 && coord.mac.active != -1; i++) zb_coord_tx_done(&coord, MAC_TX_OK, false);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!ready) setup();
    if (size < 1) return 0;
    const uint8_t *p = data + 1;
    size_t n = size - 1;
    switch (data[0] & 3u) {
    case 0:
        if (n >= 1 && n <= MAC_MAX_MPDU) zb_coord_rx(&coord, p, (uint8_t)n, 200, -40);
        break;
    case 1: {
        nwk_header_t h;
        if (n > CORE_SECURITY_MAX_FRAME - 18u || nwk_parse(p, n, &h) != NWK_OK || h.header_len > n) break;
        uint8_t frame[CORE_SECURITY_MAX_FRAME];
        memcpy(frame, p, n);
        uint8_t local[8];
        for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(DEV_EXT >> (8 * i));
        size_t total = 0;
        if (core_security_seal(&dev_sec, 0, local, frame, h.header_len, n - h.header_len, sizeof frame, &total))
            air(frame, total, (h.src & 1u) ? h.src : DEV_SHORT);
        break;
    }
    case 2:
        if (n >= 2 && n - 2 <= 250) znp_handle(&znp, p[0], p[1], p + 2, (uint8_t)(n - 2));
        break;
    default:
        now_ms += n ? (uint32_t)p[0] * 100u : 1000u;
        zb_coord_tick(&coord, now_ms);
        break;
    }
    drain();
    return 0;
}
