/* SPDX-License-Identifier: MIT
 * Sleepy end devices (children of the coordinator): data polling, indirect
 * queues, frame-pending bit, quotas and saturation, expiry, broadcast copies,
 * End Device Timeout, and a randomized run whose invariant is: every frame
 * accepted for a sleepy child is delivered exactly once, to that child, on
 * its poll, or expires with a TRANSACTION_EXPIRED confirmation; while the
 * indirect queues are saturated, direct traffic still goes out.
 */
#include "frt_test.h"
#include "zb_ccm_backend.h"
#include "zb_coord.h"

#define PAN 0x5E5E
#define CHILDREN 24u
static const uint8_t KEY[16] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40};
static zb_coord_t coord;
static uint32_t now = 1;

/* ---- radio ------------------------------------------------------------ */
static uint8_t air[64][MAC_MAX_MPDU];
static uint8_t air_len[64];
static unsigned air_head, air_tail;
static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx; (void)ack;
    memcpy(air[air_tail % 64], mpdu, len);
    air_len[air_tail % 64] = len;
    air_tail++;
    return 0;
}
static bool pending_bit[0x10000];
static void sim_match(void *ctx, bool ext, uint64_t a, bool p) { (void)ctx; if (!ext) pending_bit[a & 0xFFFF] = p; }
/* Next transmission (acknowledged); false if none. */
static bool take(mac_frame_t *f, uint8_t *copy) {
    if (air_head == air_tail) return false;
    unsigned i = air_head++ % 64;
    memcpy(copy, air[i], air_len[i]);
    CHECK(mac_frame_parse(copy, air_len[i], f) == MAC_OK);
    zb_coord_tx_done(&coord, MAC_TX_OK, false);
    return true;
}

/* ---- confirmations (per MAC handle) ---------------------------------- */
static unsigned confirms_ok, confirms_expired, confirms_other;
static bool tracking, tracked[256];
static void up_confirm(void *ctx, uint8_t handle, uint8_t status) {
    (void)ctx;
    if (tracking) {                       /* random run: only the children's frames */
        if (!tracked[handle]) return;
        tracked[handle] = false;
    }
    if (status == MAC_STATUS_SUCCESS) confirms_ok++;
    else if (status == MAC_STATUS_TRANSACTION_EXPIRED) confirms_expired++;
    else confirms_other++;
}
static bool reserve(void *ctx, uint32_t u) { (void)ctx; (void)u; return true; }

/* ---- children ---------------------------------------------------------- */
typedef struct { uint16_t short_addr; uint64_t ext; uint8_t mac_seq; core_security_t sec; } child_t;
static child_t kid[CHILDREN];
static void poll(child_t *k) {
    const uint8_t cmd = MAC_CMD_DATA_REQUEST;
    mac_frame_t f = {0};
    f.type = MAC_TYPE_COMMAND; f.ack_request = true; f.pan_id_compression = true; f.seq = k->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = k->short_addr;
    uint8_t mpdu[32];
    size_t n = mac_frame_build(&f, &cmd, 1, mpdu, sizeof mpdu);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
}
/* Decrypt the NWK frame as the child; returns the first payload byte (ZCL
 * transaction number we put there) or -1. */
static int open_as(child_t *k, const mac_frame_t *f, bool *is_command) {
    uint8_t frame[MAC_MAX_MPDU];
    memcpy(frame, f->payload, f->payload_len);
    nwk_header_t h;
    if (nwk_parse(frame, f->payload_len, &h) != NWK_OK || !h.security) return -1;
    size_t plen;
    if (!core_security_open_authenticated_peer(&k->sec, frame, f->payload_len, h.header_len, &plen)) return -1;
    const uint8_t *p = frame + h.header_len + 14;
    *is_command = h.type == NWK_TYPE_COMMAND;
    if (*is_command) return p[0];
    aps_header_t a;
    if (aps_parse(p, plen, &a) != APS_OK || a.payload_len < 2) return -1;
    return a.payload[1];
}
static uint8_t send_to(child_t *k, uint8_t tag) {
    const uint8_t zcl[3] = {0x01, tag, 0x02};
    return zb_coord_send_aps(&coord, k->short_addr, 1, 1, 6, 0x0104, zcl, 3, false);
}

int main(void) {
    static const mac_radio_ops_t RADIO = {sim_tx, sim_match, 0};
    zb_coord_upper_t upper = {0};
    upper.reserve_nwk_counter = reserve;
    upper.send_confirm = up_confirm;
    zb_coord_init(&coord, &RADIO, &upper, 0x00124B0000005E5Eull, 0xC0FFEEu, 1);
    const uint8_t epid[8] = {5, 5, 5, 5, 5, 5, 5, 5};
    zb_coord_form(&coord, PAN, 11, epid, KEY, 0);
    zb_coord_tick(&coord, now);
    for (unsigned i = 0; i < CHILDREN; i++) {
        kid[i].short_addr = (uint16_t)(0x3000 + i);
        kid[i].ext = 0xA4C1380000200000ull + i;
        core_security_init(&kid[i].sec, 1, zb_ccm_backend_sw, reserve, 0);
        CHECK(core_security_set_key(&kid[i].sec, 0, 0, KEY));
        CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, kid[i].ext, kid[i].short_addr, true, 0, 0xFF));
        nwk_device_t *d = &coord.devices.dev[nwk_devices_find_ext(&coord.devices, kid[i].ext)];
        d->capability = 0x80;               /* sleepy end device, known (it announced) */
        d->capability_guessed = false;
    }
    /* A listening router child for direct traffic. */
    CHECK(zb_coord_import_device(&coord, NWK_DEVICE_NONE, 0xA4C13800002FFFFFull, 0x7F00, true, 0, 0xFF));
    coord.devices.dev[nwk_devices_find_short(&coord.devices, 0x7F00)].capability_guessed = false;
    mac_frame_t f;
    uint8_t copy[MAC_MAX_MPDU];
    bool cmd;
    while (take(&f, copy)) {}

    /* 1. Queued until the poll; pending bit set, then cleared. */
    CHECK(send_to(&kid[0], 0x11) == ZB_SEND_OK);
    CHECK(air_head == air_tail && pending_bit[kid[0].short_addr]);
    poll(&kid[0]);
    CHECK(take(&f, copy) && f.dst_short == kid[0].short_addr && open_as(&kid[0], &f, &cmd) == 0x11 && !f.pending);
    CHECK(!pending_bit[kid[0].short_addr] && confirms_ok == 1);
    /* Empty poll: nothing sent. */
    poll(&kid[0]);
    CHECK(air_head == air_tail && coord.mac.stats.polls_empty >= 1);

    /* 2. Per-child quota (3); frame-pending bit while more is queued. */
    for (uint8_t t = 0; t < 3; t++) CHECK(send_to(&kid[1], (uint8_t)(0x20 + t)) == ZB_SEND_OK);
    CHECK(send_to(&kid[1], 0x23) == ZB_SEND_MAC_REFUSED);
    for (uint8_t t = 0; t < 3; t++) {
        poll(&kid[1]);
        CHECK(take(&f, copy) && open_as(&kid[1], &f, &cmd) == 0x20 + t);
        CHECK(f.pending == (t < 2));      /* "more data" until the last one */
    }
    CHECK(!pending_bit[kid[1].short_addr]);

    /* 3. Global saturation: never more than MAC_INDIRECT_SLOTS waiting, and
     *    direct traffic still goes out meanwhile. */
    unsigned accepted = 0, refused = 0;
    for (unsigned i = 2; i < CHILDREN; i++) {
        uint8_t st = send_to(&kid[i], 0x30);
        if (st == ZB_SEND_OK) accepted++;
        else { CHECK(st == ZB_SEND_MAC_REFUSED); refused++; }
    }
    CHECK(accepted == MAC_INDIRECT_SLOTS && refused == CHILDREN - 2 - MAC_INDIRECT_SLOTS);
    const uint8_t zcl[3] = {1, 0x40, 2};
    CHECK(zb_coord_send_aps(&coord, 0x7F00, 1, 1, 6, 0x0104, zcl, 3, false) == ZB_SEND_OK);
    CHECK(take(&f, copy) && f.dst_short == 0x7F00);   /* direct frame sent at once */

    /* 4. Expiry after macTransactionPersistenceTime: all confirmed expired,
     *    pending bits cleared, capacity back. */
    unsigned expired_before = confirms_expired;
    now += MAC_PERSISTENCE_MS + 10;
    zb_coord_tick(&coord, now);
    while (take(&f, copy)) {}                          /* periodic link status / MTORR */
    CHECK(confirms_expired == expired_before + accepted);
    for (unsigned i = 2; i < CHILDREN; i++) CHECK(!pending_bit[kid[i].short_addr]);
    CHECK(send_to(&kid[5], 0x50) == ZB_SEND_OK);
    poll(&kid[5]);
    CHECK(take(&f, copy) && open_as(&kid[5], &f, &cmd) == 0x50);

    /* 5. Broadcast to all: copies for sleepy children (bounded), on polls. */
    const uint8_t bc[3] = {0x01, 0x60, 0x02};
    uint32_t copies = coord.stats.sleepy_bcast_copies;
    CHECK(zb_coord_send_aps(&coord, NWK_BCAST_ALL, 0xFF, 1, 6, 0x0104, bc, 3, false) == ZB_SEND_OK);
    CHECK(take(&f, copy) && f.dst_short == 0xFFFF);
    CHECK(coord.stats.sleepy_bcast_copies - copies == ZB_SLEEPY_BCAST_COPIES);
    unsigned got = 0;
    for (unsigned i = 0; i < CHILDREN; i++) {
        poll(&kid[i]);
        if (take(&f, copy) && open_as(&kid[i], &f, &cmd) == 0x60) got++;
    }
    CHECK(got == ZB_SLEEPY_BCAST_COPIES);

    /* 6. End Device Timeout request: answered on the child's next poll. */
    {
        const uint8_t req[3] = {0x0B, 0x03, 0x00};
        nwk_header_t h = {0};
        h.type = NWK_TYPE_COMMAND; h.src = kid[7].short_addr; h.dst = 0x0000; h.radius = 1; h.seq = 7;
        uint8_t nwk[64];
        size_t hl = nwk_build(&h, nwk, sizeof nwk);
        memcpy(nwk + hl, req, 3);
        uint8_t local[8];
        for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(kid[7].ext >> (8 * i));
        size_t total;
        CHECK(core_security_seal(&kid[7].sec, 0, local, nwk, hl, 3, sizeof nwk, &total));
        mac_frame_t mf = {0};
        mf.type = MAC_TYPE_DATA; mf.ack_request = true; mf.pan_id_compression = true; mf.seq = kid[7].mac_seq++;
        mf.dst_mode = MAC_ADDR_SHORT; mf.dst_pan = PAN; mf.dst_short = 0;
        mf.src_mode = MAC_ADDR_SHORT; mf.src_short = kid[7].short_addr;
        uint8_t mpdu[MAC_MAX_MPDU];
        size_t n = mac_frame_build(&mf, nwk, total, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
        CHECK(coord.stats.timeout_requests == 1 && air_head == air_tail);
        poll(&kid[7]);
        CHECK(take(&f, copy) && open_as(&kid[7], &f, &cmd) == 0x0C && cmd);
    }

    /* 7. Poll from an unknown address: an empty answer, nothing else. */
    child_t ghost = {.short_addr = 0x6A6A};
    unsigned empty = coord.mac.stats.polls_empty;
    poll(&ghost);
    CHECK(air_head == air_tail && coord.mac.stats.polls_empty == empty + 1);

    /* 8. Randomized: sends, polls and time over 12 children. */
    unsigned outstanding[12] = {0}, delivered = 0, accepted_total = 0;
    uint8_t expect[12][256];
    unsigned qh[12] = {0}, qt[12] = {0};
    confirms_ok = confirms_expired = confirms_other = 0;
    tracking = true;
    for (unsigned step = 0; step < 3000; step++) {
        unsigned i = frt_rand() % 12, action = frt_rand() % 10;
        if (action < 5) {
            uint8_t tag = (uint8_t)(frt_rand() & 0x7F);
            uint8_t st = send_to(&kid[i], tag);
            if (st == ZB_SEND_OK) {
                expect[i][qt[i]++ % 256] = tag;
                outstanding[i]++;
                accepted_total++;
                tracked[coord.last_send_handle] = true;
            }
            else CHECK(st == ZB_SEND_MAC_REFUSED);
        } else if (action < 9) {
            poll(&kid[i]);
            if (take(&f, copy)) {
                CHECK(f.dst_short == kid[i].short_addr);   /* never another child's frame */
                int tag = open_as(&kid[i], &f, &cmd);
                CHECK(!cmd && outstanding[i] > 0 && tag == expect[i][qh[i]++ % 256]);   /* in order, once */
                outstanding[i]--;
                delivered++;
            }
        } else {
            uint32_t before = confirms_expired;
            now += 1000 + frt_rand() % 3000;
            zb_coord_tick(&coord, now);
            while (take(&f, copy)) CHECK(f.dst_short == 0xFFFF);   /* only periodic broadcasts */
            /* Expired transactions leave their queue head first. */
            for (unsigned e = before; e < confirms_expired; e++) {
                for (unsigned k = 0; k < 12; k++) {
                    if (!outstanding[k]) continue;
                    if (mac_pending_count(&coord.mac, kid[k].short_addr) < outstanding[k]) {
                        unsigned drop = outstanding[k] - mac_pending_count(&coord.mac, kid[k].short_addr);
                        outstanding[k] -= drop;
                        qh[k] += drop;
                    }
                }
            }
        }
    }
    CHECK(confirms_other == 0);
    CHECK(confirms_ok == delivered);
    unsigned left = 0;
    for (unsigned k = 0; k < 12; k++) left += outstanding[k];
    CHECK(delivered + confirms_expired + left == accepted_total);
    printf("sleepy: random run: %u accepted, %u delivered on polls, %u expired, %u still queued\n",
           accepted_total, delivered, confirms_expired, left);
    PASS("sleepy");
    return 0;
}
