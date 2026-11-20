/* SPDX-License-Identifier: MIT
 * Reliability fixes found with the network benchmark (sim/net_bench.c):
 *  1. MAC handles stay unique while a frame awaits its confirmation (indirect
 *     frames wait 7.68 s; the 8-bit counter wrapped meanwhile);
 *  2. ZCL reads followed one by one (destination + ZCL sequence number): a
 *     lost answer is re-requested although newer reads went to the device,
 *     a second answer to a re-sent read is not passed up twice;
 *  3. no re-send to a device known to sleep; capability learnt from a
 *     Node Descriptor response;
 *  4. an unanswered read rebuilds the route only when the destination did
 *     not answer lately (congestion is not a broken route);
 *  5. a device polling us with its short address is our child;
 *  6. 12 extended-address transactions (mass rejoin after a parent loss),
 *     12 poll answers queued at once (simultaneous wake-ups);
 *  7. a broadcast never reuses a NWK sequence number still in the
 *     broadcast transaction tables;
 *  8. rejections of the native CCM* re-verified in software within a budget
 *     (a forger cannot hold the CPU), silent faults still caught.
 */
#include "frt_test.h"
#include "zb_ccm_backend.h"
#include "zb_coord.h"
#include "znp.h"
#include "nv_log.h"

#define PAN 0x7E57
static const uint8_t KEY[16] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18, 0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90};
static zb_coord_t coord;
static uint32_t now = 1;

/* ---- radio: transmissions captured, completed on demand ---------------- */
static uint8_t air[256][MAC_MAX_MPDU];
static uint8_t air_len[256];
static unsigned air_head, air_tail;
static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx; (void)ack;
    memcpy(air[air_tail % 256], mpdu, len);
    air_len[air_tail % 256] = len;
    air_tail++;
    return 0;
}
static bool pending_bit[0x10000];
static void sim_match(void *ctx, bool ext, uint64_t a, bool p) { (void)ctx; if (!ext) pending_bit[a & 0xFFFF] = p; }
static core_security_t peer_sec;   /* any network member: reads what we send */
typedef struct { mac_frame_t mac; nwk_header_t nwk; uint8_t plain[MAC_MAX_MPDU]; size_t plen; bool ok; } seen_t;
/* Completes the transmission in flight (acknowledged) and decodes it. */
static bool take(seen_t *s) {
    if (air_head == air_tail) return false;
    static uint8_t copy[MAC_MAX_MPDU];
    unsigned i = air_head++ % 256;
    memcpy(copy, air[i], air_len[i]);
    zb_coord_tx_done(&coord, MAC_TX_OK, false);
    memset(s, 0, sizeof *s);
    CHECK(mac_frame_parse(copy, air_len[i], &s->mac) == MAC_OK);
    if (s->mac.type != MAC_TYPE_DATA) return true;
    memcpy(s->plain, s->mac.payload, s->mac.payload_len);
    if (nwk_parse(s->plain, s->mac.payload_len, &s->nwk) != NWK_OK || !s->nwk.security) return true;
    s->ok = core_security_open_authenticated_peer(&peer_sec, s->plain, s->mac.payload_len, s->nwk.header_len, &s->plen);
    return true;
}
static unsigned drain(unsigned *rreqs) {
    seen_t s;
    unsigned n = 0;
    while (take(&s)) {
        n++;
        if (rreqs && s.ok && s.nwk.type == NWK_TYPE_COMMAND && s.nwk.dst == 0xFFFC &&
            s.plain[s.nwk.header_len + 14] == 0x01 && !(s.plain[s.nwk.header_len + 15] & 0x18))
            (*rreqs)++;
    }
    return n;
}

/* ---- upper layer --------------------------------------------------------- */
static unsigned up_frames, up_last_tsn;
static void on_aps(void *ctx, const nwk_header_t *n, const aps_header_t *a, const uint8_t *p, size_t l, uint8_t q) {
    (void)ctx; (void)n; (void)a; (void)q;
    up_frames++;
    if (l >= 2) up_last_tsn = p[1];
}
static unsigned confirms[256];
static void on_confirm(void *ctx, uint8_t handle, uint8_t status) { (void)ctx; (void)status; confirms[handle]++; }
static bool reserve(void *ctx, uint32_t u) { (void)ctx; (void)u; return true; }

/* ---- devices ---------------------------------------------------------------- */
typedef struct { uint16_t short_addr; uint64_t ext; uint8_t mac_seq, nwk_seq, aps; core_security_t sec; } node_dev_t;
static node_dev_t router[4], sleeper[16];
static void dev_init(node_dev_t *d, uint16_t s, uint64_t ext, bool child, uint8_t cap, bool known) {
    d->short_addr = s;
    d->ext = ext;
    core_security_init(&d->sec, 1, zb_ccm_backend_sw, reserve, 0);
    CHECK(core_security_set_key(&d->sec, 0, 0, KEY));
    CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, ext, s, child, 0, 0xFF));
    nwk_device_t *e = &coord.devices.dev[nwk_devices_find_ext(&coord.devices, ext)];
    e->capability = cap;
    e->capability_guessed = !known;
}
/* Device `d` sends us APS data (MAC source = itself, one hop). */
static void from_dev(node_dev_t *d, const uint8_t *aps, size_t len) {
    nwk_header_t h = {0};
    h.type = NWK_TYPE_DATA; h.src = d->short_addr; h.dst = 0x0000; h.radius = 30; h.seq = d->nwk_seq++;
    uint8_t nwk[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(&h, nwk, sizeof nwk);
    memcpy(nwk + hl, aps, len);
    uint8_t local[8];
    for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(d->ext >> (8 * i));
    size_t total;
    CHECK(core_security_seal(&d->sec, 0, local, nwk, hl, len, sizeof nwk, &total));
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.ack_request = true; f.pan_id_compression = true; f.seq = d->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = d->short_addr;
    uint8_t mpdu[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&f, nwk, total, mpdu, sizeof mpdu);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
}
/* NWK frame to us secured by `sealer`, MAC source `mac_src`, NWK source `src`. */
static void nwk_from(node_dev_t *sealer, uint16_t mac_src, uint16_t src, uint8_t type, const uint8_t *p, size_t len) {
    nwk_header_t h = {0};
    h.type = type; h.src = src; h.dst = 0x0000; h.radius = 30; h.seq = sealer->nwk_seq++;
    uint8_t nwk[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(&h, nwk, sizeof nwk);
    memcpy(nwk + hl, p, len);
    uint8_t local[8];
    for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(sealer->ext >> (8 * i));
    size_t total;
    CHECK(core_security_seal(&sealer->sec, 0, local, nwk, hl, len, sizeof nwk, &total));
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.ack_request = true; f.pan_id_compression = true; f.seq = sealer->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = mac_src;
    uint8_t mpdu[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&f, nwk, total, mpdu, sizeof mpdu);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
}
/* Same frame built for another coordinator (returns the MPDU length). */
static size_t build_from(node_dev_t *sealer, uint16_t mac_src, uint16_t src, uint8_t type, const uint8_t *p, size_t len,
                         uint8_t *mpdu) {
    nwk_header_t h = {0};
    h.type = type; h.src = src; h.dst = 0x0000; h.radius = 30; h.seq = sealer->nwk_seq++;
    uint8_t nwk[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(&h, nwk, sizeof nwk);
    memcpy(nwk + hl, p, len);
    uint8_t local[8];
    for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(sealer->ext >> (8 * i));
    size_t total;
    CHECK(core_security_seal(&sealer->sec, 0, local, nwk, hl, len, sizeof nwk, &total));
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.ack_request = true; f.pan_id_compression = true; f.seq = sealer->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = mac_src;
    return mac_frame_build(&f, nwk, total, mpdu, MAC_MAX_MPDU);
}
static void dev_init_on(zb_coord_t *c, node_dev_t *d, uint16_t s, uint64_t ext) {
    d->short_addr = s;
    d->ext = ext;
    core_security_init(&d->sec, 1, zb_ccm_backend_sw, reserve, 0);
    CHECK(core_security_set_key(&d->sec, 0, 0, KEY));
    CHECK(zb_coord_import_device(c, NWK_DEVICE_NONE, ext, s, false, 0, 0xFF));
}
/* NOR flash in RAM for a store (2 banks of 16 KB, 8 KB sectors). */
#define NOR_BANK 0x4000u
#define NOR_SECTOR 0x2000u
static uint8_t nor[2 * NOR_BANK];
static bool nor_read(void *c, uint32_t a, void *b, size_t n) {
    (void)c;
    if (a + n > sizeof nor) return false;
    memcpy(b, nor + a, n);
    return true;
}
static bool nor_program(void *c, uint32_t a, const void *d, size_t n) {
    (void)c;
    if (a + n > sizeof nor) return false;
    for (size_t i = 0; i < n; i++) nor[a + i] &= ((const uint8_t *)d)[i];
    return true;
}
static bool nor_erase(void *c, uint32_t a) {
    (void)c;
    if (a + NOR_SECTOR > sizeof nor) return false;
    memset(nor + a, 0xFF, NOR_SECTOR);
    return true;
}
static void cmd_from(node_dev_t *d, const uint8_t *p, size_t len) {
    nwk_from(d, d->short_addr, d->short_addr, NWK_TYPE_COMMAND, p, len);
}
static void relayed_from(node_dev_t *relay, uint16_t src, const uint8_t *aps, size_t len) {
    nwk_from(relay, relay->short_addr, src, NWK_TYPE_DATA, aps, len);
}
/* ZNP output of the test's own ZNP instance: last frame kept. */
static uint8_t znp_last[256], znp_c0, znp_c1;
static void znp_out(void *ctx, uint8_t c0, uint8_t c1, const uint8_t *p, uint8_t n) {
    (void)ctx;
    znp_c0 = c0;
    znp_c1 = c1;
    memcpy(znp_last, p, n);
}
static uint8_t tl_last_channel;
static bool tl_channel(void *ctx, uint8_t channel) { (void)ctx; tl_last_channel = channel; return true; }
static void read_answer(node_dev_t *d, uint8_t tsn) {
    const uint8_t aps[8 + 8] = {0x00, 0x01, 0x00, 0x00, 0x04, 0x01, 0x01, d->aps++,
                                0x18, tsn, 0x01, 0x05, 0x00, 0x00, 0x20, 0x01};
    from_dev(d, aps, sizeof aps);
}
static void poll(node_dev_t *d) {
    const uint8_t cmd = MAC_CMD_DATA_REQUEST;
    mac_frame_t f = {0};
    f.type = MAC_TYPE_COMMAND; f.ack_request = true; f.pan_id_compression = true; f.seq = d->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = d->short_addr;
    uint8_t mpdu[32];
    size_t n = mac_frame_build(&f, &cmd, 1, mpdu, sizeof mpdu);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
}
static uint8_t read_to(node_dev_t *d, uint8_t tsn) {
    const uint8_t zcl[5] = {0x10, tsn, 0x00, 0x05, 0x00};   /* read attribute 0x0005 */
    return zb_coord_send_aps(&coord, d->short_addr, 1, 1, 0x0000, 0x0104, zcl, 5, false);
}
static void advance(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) { now += 10; zb_coord_tick(&coord, now); }
}

/* ---- CCM*: fake native engine (software result, optionally lying) -------- */
static bool native_rejects_all;
static bool fake_native(const uint8_t key[16], bool encrypt, const uint8_t nonce[13], const uint8_t *aad, unsigned aad_len,
                        const uint8_t *in, unsigned len, uint8_t *out, uint8_t tag[4]) {
    if (encrypt) return zb_ccm_backend_sw(0, true, key, nonce, aad, aad_len, in, len, out, tag);
    /* decrypt: the backend compares `tag` with the frame's MIC; the true MIC
     * is the one sealing the plaintext again. */
    /* CTR is symmetric: "encrypting" the ciphertext gives the plaintext;
     * sealing that plaintext gives the frame's true MIC. */
    uint8_t mic[4], tmp_mic[4], plain[128], ct[128];
    if (len > sizeof plain) return false;
    (void)zb_ccm_backend_sw(0, true, key, nonce, aad, aad_len, in, len, plain, tmp_mic);
    (void)zb_ccm_backend_sw(0, true, key, nonce, aad, aad_len, plain, len, ct, mic);
    memcpy(out, plain, len);
    for (unsigned i = 0; i < 4; i++) tag[i] = (uint8_t)(native_rejects_all ? (mic[i] ^ 0x5A) : mic[i]);
    return true;
}

int main(void) {
    static const mac_radio_ops_t RADIO = {sim_tx, sim_match, 0};
    zb_coord_upper_t upper = {0};
    upper.reserve_nwk_counter = reserve;
    upper.send_confirm = on_confirm;
    upper.aps_data = on_aps;
    zb_coord_init(&coord, &RADIO, &upper, 0x00124B0000007E57ull, 0xBADC0DEu, 1);
    const uint8_t epid[8] = {7, 7, 7, 7, 7, 7, 7, 7};
    zb_coord_form(&coord, PAN, 15, epid, KEY, 0);
    zb_coord_tick(&coord, now);
    core_security_init(&peer_sec, 1, zb_ccm_backend_sw, reserve, 0);
    CHECK(core_security_set_key(&peer_sec, 0, 0, KEY));
    for (unsigned i = 0; i < 4; i++)
        dev_init(&router[i], (uint16_t)(0x1100 + i), 0x00158D0000E00000ull + i, false, 0x8E, true);
    for (unsigned i = 0; i < 16; i++)
        dev_init(&sleeper[i], (uint16_t)(0x2200 + i), 0x00158D0000F00000ull + i, true, 0x80, true);
    /* router[0..3] one hop (neighbours), router[3] behind router[2] */
    for (unsigned i = 0; i < 3; i++) {
        const uint8_t ls[2] = {0x08, 0x60};
        nwk_header_t h = {0};
        h.type = NWK_TYPE_COMMAND; h.src = router[i].short_addr; h.dst = 0xFFFC; h.radius = 1;
        h.seq = router[i].nwk_seq++;
        uint8_t nwk[CORE_SECURITY_MAX_FRAME];
        size_t hl = nwk_build(&h, nwk, sizeof nwk);
        memcpy(nwk + hl, ls, 2);
        uint8_t local[8];
        for (unsigned k = 0; k < 8; k++) local[k] = (uint8_t)(router[i].ext >> (8 * k));
        size_t total;
        CHECK(core_security_seal(&router[i].sec, 0, local, nwk, hl, 2, sizeof nwk, &total));
        mac_frame_t f = {0};
        f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.seq = router[i].mac_seq++;
        f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0xFFFF;
        f.src_mode = MAC_ADDR_SHORT; f.src_short = router[i].short_addr;
        uint8_t mpdu[MAC_MAX_MPDU];
        size_t n = mac_frame_build(&f, nwk, total, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
    }
    const uint16_t via = router[2].short_addr;
    nwk_source_route_set(&coord.routes, router[3].short_addr, &via, 1);
    advance(4000);   /* first link status / many-to-one request */
    drain(0);

    /* 1. Handles: a frame for a sleepy child waits for its poll; 600 frames
     *    to a router go out and are confirmed meanwhile (the 8-bit counter
     *    wraps twice): none takes the pending handle, each confirm once. */
    {
        CHECK(read_to(&sleeper[0], 0x01) == ZB_SEND_OK);
        uint8_t pending = coord.last_send_handle;
        unsigned before[256];
        memcpy(before, confirms, sizeof confirms);
        for (unsigned i = 0; i < 600; i++) {
            const uint8_t zcl[3] = {0x11, (uint8_t)i, 0x0B};   /* not a read: no follow-up */
            CHECK(zb_coord_send_aps(&coord, router[0].short_addr, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
            CHECK(coord.last_send_handle != pending);
            drain(0);
        }
        CHECK(confirms[pending] == before[pending]);   /* not confirmed by another frame */
        poll(&sleeper[0]);
        seen_t s;
        CHECK(take(&s) && s.mac.dst_short == sleeper[0].short_addr);
        CHECK(confirms[pending] == before[pending] + 1);
        printf("reliability: 600 frames sent while an indirect frame waited, its handle never reused\n");
    }

    /* 2. Reads followed one by one: A then B to the same router; only B is
     *    answered; A is re-requested at 1.1 s (it was forgotten when B was
     *    sent). A's two answers reach the host once. */
    {
        unsigned up0 = up_frames;
        CHECK(read_to(&router[1], 0x21) == ZB_SEND_OK);
        CHECK(read_to(&router[1], 0x22) == ZB_SEND_OK);
        CHECK(drain(0) == 2);
        read_answer(&router[1], 0x22);
        CHECK(up_frames == up0 + 1 && up_last_tsn == 0x22);
        uint32_t retx0 = coord.stats.net_status_retries;
        advance(1200);
        seen_t s;
        unsigned resent = 0;
        while (take(&s)) {
            if (s.ok && s.mac.dst_short == router[1].short_addr && s.nwk.type == NWK_TYPE_DATA) {
                CHECK(s.plain[s.nwk.header_len + 14 + 9] == 0x21);   /* A, not B */
                resent++;
            }
        }
        CHECK(resent == 1 && coord.stats.net_status_retries == retx0 + 1);
        read_answer(&router[1], 0x21);   /* late answer to the first request */
        read_answer(&router[1], 0x21);   /* answer to the re-sent one */
        CHECK(up_frames == up0 + 2 && coord.stats.zcl_answers_dropped == 1);
        advance(5000);
        drain(0);
        printf("reliability: lost answer re-requested despite a newer read, second answer dropped\n");
    }

    /* 3. Known sleeper behind a router: no re-send; capability learnt from a
     *    Node Descriptor response for an imported (guessed) device. */
    {
        node_dev_t far_sleeper;
        dev_init(&far_sleeper, 0x3300, 0x00158D0000AB0001ull, false, 0x80, true);
        const uint16_t parent = router[0].short_addr;
        nwk_source_route_set(&coord.routes, far_sleeper.short_addr, &parent, 1);
        CHECK(read_to(&far_sleeper, 0x31) == ZB_SEND_OK);
        drain(0);
        uint32_t sk0 = coord.stats.zcl_retx_sleepy_skipped;
        advance(5000);
        unsigned rreq = 0;
        (void)drain(&rreq);
        CHECK(rreq == 0 && coord.stats.zcl_retx_sleepy_skipped == sk0 + 1);
        node_dev_t guessed;
        dev_init(&guessed, 0x3301, 0x00158D0000AB0002ull, false, NWK_CAP_RX_ON_IDLE, false);
        /* Node_Desc_rsp: tsn, status, nwk addr, descriptor (MAC capability 0x80) */
        const uint8_t aps[8 + 17] = {0x00, 0x00, 0x02, 0x80, 0x00, 0x00, 0x00, guessed.aps++,
                                     0x44, 0x00, 0x01, 0x33, 0x02, 0x40, 0x80, 0x00, 0x00, 0x50, 0xA0, 0x00, 0x00, 0x00,
                                     0xA0, 0x00, 0x00};
        from_dev(&guessed, aps, sizeof aps);
        const nwk_device_t *g = &coord.devices.dev[nwk_devices_find_short(&coord.devices, 0x3301)];
        CHECK(!g->capability_guessed && g->capability == 0x80 && coord.stats.capabilities_learnt == 1);
        printf("reliability: no re-send to known sleepers, capability learnt from Node_Desc_rsp\n");
    }

    /* 4. Route rebuild only for destinations that did not answer lately.
     *    router[3] (behind router[2]) answered a read: its next unanswered
     *    read is re-sent twice, its source route kept, no Route Request.
     *    router[0] never answered: the second miss rebuilds its route. */
    {
        CHECK(read_to(&router[3], 0x41) == ZB_SEND_OK);
        drain(0);
        read_answer(&router[3], 0x41);
        CHECK(read_to(&router[3], 0x42) == ZB_SEND_OK);
        drain(0);
        uint32_t rb0 = coord.stats.zcl_route_rebuilds;
        unsigned rreq = 0;
        for (unsigned t = 0; t < 45; t++) { advance(100); drain(&rreq); }
        CHECK(rreq == 0 && coord.stats.zcl_route_rebuilds == rb0);
        CHECK(nwk_source_route(&coord.routes, router[3].short_addr) != 0);
        advance(ZB_ANSWERED_MS);   /* router[0]'s answers (none) and router[3]'s age out */
        drain(0);
        CHECK(read_to(&router[0], 0x43) == ZB_SEND_OK);
        drain(0);
        uint32_t rq0 = coord.stats.rreq_tx;
        for (unsigned t = 0; t < 30; t++) { advance(100); drain(0); }
        CHECK(coord.stats.zcl_route_rebuilds == rb0 + 1 && coord.stats.rreq_tx == rq0 + 1);
        advance(ZB_DISC_WAIT_MS + 2000);
        drain(0);
        printf("reliability: route kept for a destination that answered lately, rebuilt otherwise\n");
    }

    /* 5. A device polling us with its short address is our child, even when
     *    we had it behind a router (frames then go indirect). */
    {
        node_dev_t moved;
        dev_init(&moved, 0x3400, 0x00158D0000AC0001ull, false, 0x80, true);
        const uint16_t parent = router[0].short_addr;
        nwk_source_route_set(&coord.routes, moved.short_addr, &parent, 1);
        poll(&moved);
        drain(0);
        const nwk_device_t *m = &coord.devices.dev[nwk_devices_find_short(&coord.devices, 0x3400)];
        CHECK(m->relationship == NWK_REL_CHILD && coord.stats.poll_children == 1);
        CHECK(read_to(&moved, 0x51) == ZB_SEND_OK);
        CHECK(air_head == air_tail && pending_bit[moved.short_addr]);   /* held for its poll */
        poll(&moved);
        seen_t s;
        CHECK(take(&s) && s.mac.dst_short == moved.short_addr);
        printf("reliability: a device polling us becomes our child\n");
    }

    /* 6. Capacities: 12 extended-address transactions at once; 12 poll
     *    answers queued while the radio is busy. */
    {
        unsigned ok = 0;
        for (unsigned i = 0; i < MAC_EXT_INDIRECT + 1; i++)
            ok += mac_association_response(&coord.mac, 0x00158D0000AD0000ull + i, (uint16_t)(0x3500 + i), 0) ==
                  MAC_STATUS_SUCCESS;
        CHECK(MAC_EXT_INDIRECT == 12u && ok == MAC_EXT_INDIRECT);
        advance(MAC_PERSISTENCE_MS + 100);   /* expire them */
        drain(0);
        for (unsigned i = 1; i < 14; i++) CHECK(read_to(&sleeper[i], (uint8_t)(0x60 + i)) == ZB_SEND_OK);
        const uint8_t busy[3] = {0x11, 0x01, 0x0B};
        CHECK(zb_coord_send_aps(&coord, router[0].short_addr, 1, 1, 6, 0x0104, busy, 3, false) == ZB_SEND_OK);
        uint32_t full0 = coord.mac.stats.poll_queue_full;
        for (unsigned i = 1; i < 14; i++) poll(&sleeper[i]);   /* radio still busy with the direct frame */
        CHECK(coord.mac.urgent_count == MAC_URGENT_QUEUE && coord.mac.stats.poll_queue_full == full0 + 1);
        seen_t s;
        unsigned served = 0;
        while (take(&s)) served += s.mac.dst_short >= 0x2201 && s.mac.dst_short <= 0x220D;
        CHECK(served == MAC_URGENT_QUEUE);
        printf("reliability: %u rejoin/association responses and %u poll answers queued at once\n", MAC_EXT_INDIRECT,
               MAC_URGENT_QUEUE);
    }

    /* 7. Broadcast sequence numbers: after 256 unicasts the counter is back
     *    on the number of our last broadcast, still in the BTT: skipped. */
    {
        advance(MAC_PERSISTENCE_MS + 100);
        drain(0);
        CHECK(zb_coord_discover_route(&coord, 0x4777));
        seen_t s;
        uint8_t used = 0;
        bool found = false;
        while (take(&s)) if (s.nwk.dst == 0xFFFC) { used = s.nwk.seq; found = true; }
        CHECK(found);
        coord.nwk_seq = used;   /* as after 256 frames */
        uint32_t sk0 = coord.stats.bcast_seq_skipped;
        CHECK(zb_coord_discover_route(&coord, 0x4778));
        found = false;
        while (take(&s)) if (s.nwk.dst == 0xFFFC && s.ok) { CHECK(s.nwk.seq != used); found = true; }
        CHECK(found && coord.stats.bcast_seq_skipped == sk0 + 1);
        printf("reliability: broadcast sequence number still in the BTT skipped\n");
    }

    /* 9. Children that leave: (a) aged after the End Device Timeout they
     *    requested, kept while they poll; (b) one heard through a router is
     *    no longer ours; (c) ZNP assocRemove / assocAdd (herdsman recovery). */
    {
        node_dev_t *quiet = &sleeper[14], *alive = &sleeper[15];
        const uint8_t req[3] = {0x0B, 0x00 /* 10 s */, 0x00};
        cmd_from(quiet, req, sizeof req);
        cmd_from(alive, req, sizeof req);
        poll(quiet);
        poll(alive);
        drain(0);
        CHECK(coord.stats.timeout_requests >= 2);
        for (unsigned t = 0; t < 8; t++) { advance(5000); poll(alive); drain(0); }   /* 40 s */
        const nwk_device_t *q = &coord.devices.dev[nwk_devices_find_short(&coord.devices, quiet->short_addr)];
        const nwk_device_t *a = &coord.devices.dev[nwk_devices_find_short(&coord.devices, alive->short_addr)];
        CHECK(q->relationship == NWK_REL_NONE && a->relationship == NWK_REL_CHILD && coord.stats.children_aged == 1);
        const uint8_t bad[3] = {0x0B, 15, 0x00};   /* out of range: refused, nothing stored */
        cmd_from(alive, bad, sizeof bad);
        poll(alive);
        seen_t s;
        bool refused = false;
        while (take(&s))
            if (s.ok && s.nwk.type == NWK_TYPE_COMMAND && s.plain[s.nwk.header_len + 14] == 0x0C)
                refused = s.plain[s.nwk.header_len + 15] == 0x01;
        CHECK(refused);

        node_dev_t *moved = &sleeper[13];
        const uint8_t aps[8 + 3] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, moved->aps++, 0x18, 0x01, 0x0A};
        relayed_from(&router[0], moved->short_addr, aps, sizeof aps);
        const nwk_device_t *m = &coord.devices.dev[nwk_devices_find_short(&coord.devices, moved->short_addr)];
        CHECK(m->relationship == NWK_REL_NONE && coord.stats.children_moved == 1);
        drain(0);

        /* ZNP, on a coordinator of its own (its frames are never taken). */
        static znp_t znp;
        static zb_coord_t zc;
        static const znp_platform_t PLAT = {.send = znp_out, .product = ZNP_PRODUCT_ZSTACK3X0, .revision = 20261120u};
        znp_boot(&znp, &PLAT, &zc, &RADIO, 0, 0x00124B0000009999ull, 0x99u, 0);
        zc.upper.reserve_nwk_counter = reserve;
        zb_coord_form(&zc, 0x9999, 20, epid, KEY, 0);
        znp.formed = true;
        znp.dev_state = ZNP_DEV_ZB_COORD;
        const uint64_t ext = 0x00158D0000BB0001ull;
        CHECK(zb_coord_import_device(&zc, NWK_DEVICE_NONE, ext, 0x4321, true, 0, 0xFF));
        uint8_t p[11];
        for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(ext >> (8 * i));
        znp_handle(&znp, 0x27, 0x4A, (const uint8_t[10]){p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], 0x21, 0x43}, 10);
        CHECK(znp_c0 == 0x67 && znp_c1 == 0x4A && znp_last[4] == 0x02);   /* CHILD_RFD_RX_IDLE (guessed rx-on) */
        znp_handle(&znp, 0x27, 0x63, p, 8);                               /* assocRemove */
        CHECK(znp_c0 == 0x67 && znp_c1 == 0x63 && znp_last[0] == 0x00);
        CHECK(zc.devices.dev[nwk_devices_find_ext(&zc.devices, ext)].relationship == NWK_REL_NONE);
        p[8] = 0x21; p[9] = 0x43; p[10] = 0x01;                            /* assocAdd(ieee, 0x4321, CHILD_RFD) */
        znp_handle(&znp, 0x27, 0x64, p, 11);
        CHECK(znp_c0 == 0x67 && znp_c1 == 0x64 && znp_last[0] == 0x00);
        const nwk_device_t *r = &zc.devices.dev[nwk_devices_find_ext(&zc.devices, ext)];
        CHECK(r->relationship == NWK_REL_CHILD && !r->capability_guessed && !(r->capability & NWK_CAP_RX_ON_IDLE));
        p[0] ^= 0xFF;                                                      /* unknown device */
        znp_handle(&znp, 0x27, 0x63, p, 8);
        CHECK(znp_c1 == 0x63 && znp_last[0] == 0x01);
        air_head = air_tail;
        printf("reliability: child aged after its End Device Timeout, child heard via a router dropped, "
               "assocRemove/assocAdd served\n");
    }

    /* 10. 45 neighbour routers kept (the table held 32); Mgmt_Rtg_rsp pages
     *     through next-hop and source routes, no entry lost or repeated. */
    {
        nwk_routes_t *rt = &coord.routes;
        for (unsigned i = 0; i < 45; i++) {
            const uint8_t none[3] = {0};
            (void)nwk_neighbor_link_status(rt, (uint16_t)(0x5000 + i), 220, 0x0000, none, 0, false);
        }
        unsigned used = 0;
        for (unsigned i = 0; i < NWK_NEIGHBORS; i++) used += rt->neighbors[i].used && rt->neighbors[i].addr >= 0x5000;
        CHECK(NWK_NEIGHBORS >= 48u && used == 45);
        for (unsigned i = 0; i < 40; i++) nwk_table_route_set(rt, (uint16_t)(0x6000 + i), 0x5000);
        unsigned expected = 0;
        for (unsigned i = 0; i < NWK_TABLE_ROUTES; i++) expected += rt->table[i].used;
        for (unsigned i = 0; i < NWK_SOURCE_ROUTES; i++) expected += rt->source[i].used;
        CHECK(expected >= 40);
        uint8_t out[128];
        unsigned seen = 0, distinct_tables = 0;
        for (unsigned start = 0; start < expected;) {
            const uint8_t req[2] = {0x55, (uint8_t)start};
            size_t n = zb_coord_zdo_local(&coord, 0x0032, req, 2, false, out, sizeof out);
            CHECK(n >= 5 && out[1] == 0 && out[2] == (expected > 255 ? 255 : expected) && out[3] == start);
            unsigned got = out[4];
            CHECK(got > 0 && got <= 10 && n == 5u + 5u * got);
            for (unsigned k = 0; k < got; k++) {
                uint16_t dst = (uint16_t)(out[5 + 5 * k] | out[6 + 5 * k] << 8);
                distinct_tables += dst >= 0x6000 && dst < 0x6028;
            }
            seen += got;
            start += got;
        }
        CHECK(seen == expected && distinct_tables == 40);
        for (unsigned i = 0; i < 45; i++) nwk_neighbor_remove(rt, (uint16_t)(0x5000 + i));
        for (unsigned i = 0; i < 40; i++) nwk_routes_forget(rt, (uint16_t)(0x6000 + i));
        printf("reliability: 45 neighbours kept, Mgmt_Rtg pages over %u routes\n", expected);
    }

    /* 11. Incoming NWK frame counters survive a reboot (floor every 1024):
     *     an old frame replayed after the reboot is refused; a re-joined
     *     device (counters restarted) is accepted at once. */
    {
        static nv_log_t log;
        static const nv_flash_ops_t OPS = {nor_read, nor_program, nor_erase, 0};
        memset(nor, 0xFF, sizeof nor);
        CHECK(nv_log_mount(&log, &OPS, 0, NOR_BANK, NOR_SECTOR) || nv_log_format(&log));
        static zb_coord_t c1, c2;
        const uint8_t epid2[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        zb_coord_init(&c1, &RADIO, &upper, 0x00124B000000AAAAull, 0x1111u, 1);
        zb_coord_attach_store(&c1, &log);
        zb_coord_form(&c1, PAN, 25, epid2, KEY, 0);
        node_dev_t d;
        memset(&d, 0, sizeof d);
        dev_init_on(&c1, &d, 0x7777, 0x00158D0000CC0001ull);
        d.sec.tx_next = d.sec.tx_reserved = 1020;
        uint8_t old[MAC_MAX_MPDU], late[MAC_MAX_MPDU];
        size_t old_len = 0, late_len = 0;
        for (unsigned i = 0; i < 10; i++) {   /* counters 1020 .. 1029 */
            uint8_t mpdu[MAC_MAX_MPDU];
            const uint8_t aps[8 + 3] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, d.aps++, 0x18, (uint8_t)i, 0x0A};
            size_t n = build_from(&d, d.short_addr, d.short_addr, NWK_TYPE_DATA, aps, sizeof aps, mpdu);
            if (i == 2) { memcpy(old, mpdu, n); old_len = n; }    /* 1022: below the stored floor (1024) */
            if (i == 6) { memcpy(late, mpdu, n); late_len = n; }  /* 1026: inside the window */
            zb_coord_rx(&c1, mpdu, (uint8_t)n, 200, -50);
        }
        CHECK(c1.stats.nwk_auth_failures == 0 && c1.stats.nwk_counters_stored == 2);   /* first frame, then 1024 */
        air_head = air_tail;
        /* reboot */
        zb_coord_init(&c2, &RADIO, &upper, 0x00124B000000AAAAull, 0x2222u, 1);
        zb_coord_attach_store(&c2, &log);
        CHECK(zb_coord_restore(&c2) && c2.stats.nwk_counters_restored == 1);
        old[2] ^= 0x33;    /* new MAC sequence number: only NWK replay protection can stop it */
        zb_coord_rx(&c2, old, (uint8_t)old_len, 200, -50);
        CHECK(c2.stats.nwk_auth_failures == 1);
        late[2] ^= 0x33;
        zb_coord_rx(&c2, late, (uint8_t)late_len, 200, -50);   /* window of 1024: accepted (documented) */
        CHECK(c2.stats.nwk_auth_failures == 1);
        /* the device is re-paired (fresh counters): import = new life in its slot */
        CHECK(zb_coord_import_device(&c2, NWK_DEVICE_NONE, d.ext, d.short_addr, false, 0, 0xFF));
        d.sec.tx_next = d.sec.tx_reserved = 1;
        uint8_t mpdu[MAC_MAX_MPDU];
        const uint8_t aps[8 + 3] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, d.aps++, 0x18, 0x77, 0x0A};
        size_t n = build_from(&d, d.short_addr, d.short_addr, NWK_TYPE_DATA, aps, sizeof aps, mpdu);
        zb_coord_rx(&c2, mpdu, (uint8_t)n, 200, -50);
        CHECK(c2.stats.nwk_auth_failures == 1);
        air_head = air_tail;
        printf("reliability: incoming NWK counters restored after a reboot (old frame refused), reset on re-join\n");
    }

    /* 12. Touchlink through ZNP, as herdsman drives it: interPanCtl (channel,
     *     endpoint 12), Scan Request broadcast on PAN 0xFFFF, Scan Response
     *     back as AF_INCOMING_MSG_EXT; nothing passed up once restored. */
    {
        static znp_t zt;
        static zb_coord_t zc;
        static const znp_platform_t PLAT = {.send = znp_out, .product = ZNP_PRODUCT_ZSTACK3X0, .revision = 20261120u,
                                            .gp_channel = tl_channel};
        znp_boot(&zt, &PLAT, &zc, &RADIO, 0, 0x00124B000000C0DEull, 0x77u, 0);
        zc.upper.reserve_nwk_counter = reserve;
        zb_coord_form(&zc, 0x6A6A, 20, epid, KEY, 0);
        zt.formed = true;
        zt.dev_state = ZNP_DEV_ZB_COORD;
        const zb_endpoint_t ep12 = {.endpoint = 12, .profile = 0xC05E, .device_id = 5};
        CHECK(zb_coord_register_endpoint(&zc, &ep12));
        air_head = air_tail;
        uint8_t req[2] = {0x01, 15};
        znp_handle(&zt, 0x24, 0x10, req, 2);                                  /* interPanCtl: channel 15 */
        CHECK(znp_c0 == 0x64 && znp_c1 == 0x10 && znp_last[0] == 0 && tl_last_channel == 15);
        req[0] = 0x02; req[1] = 12;
        znp_handle(&zt, 0x24, 0x10, req, 2);                                  /* interPanCtl: endpoint 12 */
        CHECK(znp_last[0] == 0);
        const uint8_t scan[9] = {0x11, 0x01, 0x00, 0x78, 0x56, 0x34, 0x12, 0x02, 0x33};   /* ZCL Scan Request */
        uint8_t ext[20 + 9] = {0x02, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0xFE, 0xFF, 0xFF, 12, 0x00, 0x10, 0x42, 0x00, 30, 9, 0};
        memcpy(ext + 20, scan, sizeof scan);
        znp_handle(&zt, 0x24, 0x02, ext, sizeof ext);                         /* dataRequestExt, PAN 0xFFFF */
        CHECK(znp_c0 == 0x64 && znp_c1 == 0x02 && znp_last[0] == 0);
        CHECK(air_tail == air_head + 1);
        mac_frame_t f;
        CHECK(mac_frame_parse(air[air_head % 256], air_len[air_head % 256], &f) == MAC_OK);
        CHECK(!f.pan_id_compression && f.dst_pan == 0xFFFF && f.dst_short == 0xFFFF && f.src_mode == MAC_ADDR_EXT &&
              f.src_ext == 0x00124B000000C0DEull && f.src_pan == 0x6A6A && !f.ack_request);
        CHECK(f.payload_len == 7 + sizeof scan && f.payload[0] == 0x0B && f.payload[1] == 0x00 && f.payload[2] == 0x0B &&
              f.payload[3] == 0x00 && f.payload[4] == 0x10 && f.payload[5] == 0x5E && f.payload[6] == 0xC0 &&
              !memcmp(f.payload + 7, scan, sizeof scan));
        zb_coord_tx_done(&zc, MAC_TX_OK, false);
        air_head = air_tail;
        /* the bulb answers (unicast inter-PAN to our IEEE, from its PAN) */
        uint8_t rsp[7 + 8] = {0x0B, 0x00, 0x03, 0x00, 0x10, 0x5E, 0xC0, 0x19, 0x01, 0x01, 0x78, 0x56, 0x34, 0x12, 0x07};
        mac_frame_t m = {0};
        m.type = MAC_TYPE_DATA; m.ack_request = true; m.seq = 0x41;
        m.dst_mode = MAC_ADDR_EXT; m.dst_pan = 0x6A6A; m.dst_ext = 0x00124B000000C0DEull;
        m.src_mode = MAC_ADDR_EXT; m.src_pan = 0x1234; m.src_ext = 0x0017880100ABCDEFull;
        uint8_t mpdu[MAC_MAX_MPDU];
        size_t n = mac_frame_build(&m, rsp, sizeof rsp, mpdu, sizeof mpdu);
        CHECK(n);
        znp_c1 = 0;
        zb_coord_rx(&zc, mpdu, (uint8_t)n, 180, -60);
        CHECK(znp_c0 == 0x44 && znp_c1 == 0x82);                             /* AF_INCOMING_MSG_EXT */
        CHECK(znp_last[2] == 0x00 && znp_last[3] == 0x10 && znp_last[4] == 0x03);   /* cluster, 64-bit source */
        uint64_t src = 0;
        for (int i = 7; i >= 0; i--) src = src << 8 | znp_last[5 + i];
        CHECK(src == 0x0017880100ABCDEFull && znp_last[13] == 0xFE && znp_last[14] == 0x34 && znp_last[15] == 0x12 &&
              znp_last[16] == 12 && znp_last[17] == 0 && znp_last[18] == 180);
        CHECK(znp_last[25] == 8 && znp_last[26] == 0 && !memcmp(znp_last + 27, rsp + 7, 8));
        req[0] = 0x00;
        znp_handle(&zt, 0x24, 0x10, req, 1);                                  /* back to the network */
        CHECK(znp_last[0] == 0 && tl_last_channel == 20);
        znp_c1 = 0;
        m.seq++;
        n = mac_frame_build(&m, rsp, sizeof rsp, mpdu, sizeof mpdu);
        zb_coord_rx(&zc, mpdu, (uint8_t)n, 180, -60);
        CHECK(znp_c1 != 0x82);                                               /* no session: not passed up */
        znp_handle(&zt, 0x24, 0x02, ext, sizeof ext);
        CHECK(znp_c1 == 0x02 && znp_last[0] == 0x02 && air_tail == air_head);   /* refused, nothing sent */
        printf("reliability: Touchlink scan request sent inter-PAN, scan response passed up, session closed\n");
    }

    /* 8. Native CCM* rejections: a flood of forged frames is re-verified in
     *    software within the budget only; an engine that then rejects
     *    genuine frames is still caught (audit) and abandoned. */
    {
        zb_ccm_backend_set_native(fake_native);
        zb_ccm_backend_set_block(0);
        uint8_t key[16], nonce[13] = {1}, aad[22] = {2}, pt[30], ct[30], mic[4], out[30];
        for (unsigned i = 0; i < 16; i++) key[i] = (uint8_t)(i + 3);
        for (unsigned i = 0; i < sizeof pt; i++) pt[i] = (uint8_t)i;
        zb_ccm_hw_stats_t st;
        for (unsigned i = 0; i < 200; i++) {
            nonce[1] = (uint8_t)i;
            CHECK(zb_ccm_backend_sw(0, true, key, nonce, aad, sizeof aad, pt, sizeof pt, ct, mic));
            mic[0] ^= 0x80;   /* forged */
            CHECK(!zb_ccm_backend_hw(0, false, key, nonce, aad, sizeof aad, ct, sizeof ct, out, mic));
        }
        zb_ccm_backend_hw_stats(&st);
        CHECK(st.reject_unverified >= 200 - 8 - 4 && st.hw_audits <= 8 + 4 && !st.hw_disabled);
        native_rejects_all = true;   /* silent fault: genuine frames rejected */
        unsigned ops = 0;
        for (; ops < 400 && !st.hw_disabled; ops++) {
            nonce[1] = (uint8_t)(ops + 7);
            nonce[2] = (uint8_t)(ops >> 8);
            CHECK(zb_ccm_backend_sw(0, true, key, nonce, aad, sizeof aad, pt, sizeof pt, ct, mic));
            (void)zb_ccm_backend_hw(0, false, key, nonce, aad, sizeof aad, ct, sizeof ct, out, mic);
            zb_ccm_backend_hw_stats(&st);
        }
        CHECK(st.hw_disabled && ops <= 192);
        printf("reliability: 200 forged frames, %u re-verified in software; silent fault caught after %u operations\n",
               st.hw_audits, ops);
        zb_ccm_backend_set_native(0);
    }
    PASS("reliability");
    return 0;
}
