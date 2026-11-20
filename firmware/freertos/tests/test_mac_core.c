/* SPDX-License-Identifier: MIT
 * Coordinator MAC against a simulated radio.
 */
#include "frt_test.h"
#include "mac_core.h"

#define COORD_EXT 0x00124B0001020304ull
#define DEV_EXT 0xA4C1380000000001ull
#define PAN 0x1A62

typedef struct { uint8_t mpdu[MAC_MAX_MPDU]; uint8_t len; bool ack; } sent_t;
static sent_t sent[64];
static unsigned sent_count;
static int tx_fail;
static struct { bool ext; uint64_t addr; bool pending; } sm[64];
static unsigned sm_count;
static uint8_t confirms[64][2];
static unsigned confirm_count;
static unsigned indications, assoc_ind, orphan_ind, comm_count, polls;
static uint64_t last_assoc, last_comm_dev;
static uint8_t last_comm_status, last_cap;
static uint8_t last_payload[MAC_MAX_MPDU], last_payload_len;

static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx;
    if (tx_fail) return -1;
    if (sent_count == 64) sent_count = 0; /* stress phase: keep the latest only */
    memcpy(sent[sent_count].mpdu, mpdu, len);
    sent[sent_count].len = len;
    sent[sent_count].ack = ack;
    sent_count++;
    return 0;
}
static void sim_sm(void *ctx, bool ext, uint64_t addr, bool pending) {
    (void)ctx;
    sm[sm_count].ext = ext; sm[sm_count].addr = addr; sm[sm_count].pending = pending;
    sm_count = (sm_count + 1) % 64;
}
static void up_confirm(void *ctx, uint8_t handle, uint8_t status) {
    (void)ctx;
    confirms[confirm_count % 64][0] = handle;
    confirms[confirm_count % 64][1] = status;
    confirm_count++;
}
static void up_data(void *ctx, const mac_frame_t *f, uint8_t lqi, int8_t rssi) {
    (void)ctx; (void)lqi; (void)rssi;
    indications++;
    last_payload_len = (uint8_t)f->payload_len;
    memcpy(last_payload, f->payload, f->payload_len);
}
static void up_assoc(void *ctx, uint64_t dev, uint8_t cap, uint8_t lqi) { (void)ctx; (void)lqi; assoc_ind++; last_assoc = dev; last_cap = cap; }
static void up_orphan(void *ctx, uint64_t dev) { (void)ctx; (void)dev; orphan_ind++; }
static void up_comm(void *ctx, uint64_t dev, uint8_t status) { (void)ctx; comm_count++; last_comm_dev = dev; last_comm_status = status; }
static void up_poll(void *ctx, uint16_t s, uint64_t e, bool served) { (void)ctx; (void)s; (void)e; (void)served; polls++; }

static const mac_radio_ops_t RADIO = {sim_tx, sim_sm, 0};
static const mac_upper_t UPPER = {up_confirm, up_data, up_assoc, up_orphan, up_comm, up_poll, 0};
static mac_t mac;
static uint8_t rx_seq = 0x80;

/* Frame from a device to the coordinator. */
static void rx_from_short(uint16_t src, uint16_t dst_pan, uint16_t dst, const uint8_t *payload, uint8_t len,
                          uint8_t type, bool same_seq) {
    mac_frame_t f = {0};
    f.type = type; f.ack_request = dst != 0xFFFF; f.pan_id_compression = true;
    f.seq = same_seq ? rx_seq : ++rx_seq;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = dst_pan; f.dst_short = dst;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = src;
    uint8_t buf[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&f, payload, len, buf, sizeof buf);
    CHECK(n);
    mac_rx(&mac, buf, (uint8_t)n, 200, -50);
}
static void poll_short(uint16_t src) {
    const uint8_t cmd = MAC_CMD_DATA_REQUEST;
    rx_from_short(src, PAN, 0x0000, &cmd, 1, MAC_TYPE_COMMAND, false);
}
static void poll_ext(uint64_t src) {
    mac_frame_t f = {0};
    f.type = MAC_TYPE_COMMAND; f.ack_request = true; f.pan_id_compression = true; f.seq = ++rx_seq;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0;
    f.src_mode = MAC_ADDR_EXT; f.src_ext = src;
    const uint8_t cmd = MAC_CMD_DATA_REQUEST;
    uint8_t buf[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&f, &cmd, 1, buf, sizeof buf);
    mac_rx(&mac, buf, (uint8_t)n, 200, -50);
}
static mac_frame_t last_sent(void) {
    mac_frame_t f;
    CHECK(sent_count > 0);
    CHECK(mac_frame_parse(sent[sent_count - 1].mpdu, sent[sent_count - 1].len, &f) == MAC_OK);
    return f;
}
static uint8_t data(uint16_t dst, uint8_t handle, bool indirect) {
    const uint8_t payload[3] = {handle, 0xAA, 0x55};
    mac_data_request_t r = {.dst_mode = MAC_ADDR_SHORT, .dst_short = dst, .handle = handle,
                            .ack_request = true, .indirect = indirect, .payload = payload, .length = 3};
    return mac_data_request(&mac, &r);
}

/* A confirm callback that sends again (as route repair / Network Status do)
 * while the radio refuses every frame at once: no recursion through
 * complete() -> callback -> send -> kick -> complete(), every frame confirmed. */
static mac_t rec_mac;
static unsigned rec_depth, rec_max_depth, rec_confirms, rec_resend_left;
static void rec_confirm(void *ctx, uint8_t handle, uint8_t status) {
    (void)ctx; (void)handle;
    CHECK(status == MAC_STATUS_RADIO_ERROR);
    rec_confirms++;
    if (++rec_depth > rec_max_depth) rec_max_depth = rec_depth;
    if (rec_resend_left) {
        rec_resend_left--;
        const uint8_t payload[2] = {1, 2};
        mac_data_request_t r = {.dst_mode = MAC_ADDR_SHORT, .dst_short = 0x2222, .handle = (uint8_t)rec_confirms,
                                .ack_request = true, .payload = payload, .length = 2};
        CHECK(mac_data_request(&rec_mac, &r) == MAC_STATUS_SUCCESS);
    }
    rec_depth--;
}
static void recursion_checks(void) {
    static const mac_upper_t up = {rec_confirm, 0, 0, 0, 0, 0, 0};
    const uint8_t epid[8] = {1};
    mac_init(&rec_mac, &RADIO, &up, COORD_EXT);
    mac_start(&rec_mac, PAN, 15, epid);
    tx_fail = 1;
    rec_resend_left = 500;
    const uint8_t payload[2] = {1, 2};
    mac_data_request_t r = {.dst_mode = MAC_ADDR_SHORT, .dst_short = 0x2222, .handle = 0, .ack_request = true,
                            .payload = payload, .length = 2};
    CHECK(mac_data_request(&rec_mac, &r) == MAC_STATUS_SUCCESS);
    CHECK(rec_confirms == 501 && rec_max_depth == 1 && rec_resend_left == 0);
    CHECK(rec_mac.stats.kicks_deferred == 500 && rec_mac.active == -1);
    tx_fail = 0;
}

int main(void) {
    recursion_checks();
    const uint8_t epid[8] = {0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD};
    mac_init(&mac, &RADIO, &UPPER, COORD_EXT);
    CHECK(data(0x1234, 1, false) == MAC_STATUS_INVALID_PARAMETER);   /* not started */
    mac_start(&mac, PAN, 15, epid);
    mac_tick(&mac, 1000);

    /* Beacon request -> beacon (urgent), association permit reflected. */
    const uint8_t breq[8] = {0x03, 0x08, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, MAC_CMD_BEACON_REQUEST};
    mac_set_association_permit(&mac, true);
    mac_rx(&mac, breq, 8, 255, -40);
    CHECK(sent_count == 1 && !sent[0].ack);
    mac_frame_t f = last_sent();
    mac_zigbee_beacon_t b;
    CHECK(f.type == MAC_TYPE_BEACON && mac_parse_zigbee_beacon(&f, &b) == MAC_OK);
    CHECK(b.association_permit && b.pan_id == PAN && b.short_addr == 0 && b.ext_pan_id[0] == 0xDD);
    mac_tx_done(&mac, MAC_TX_OK, false);

    /* Association: request (ext source, PAN 0xFFFF) -> indication. */
    {
        mac_frame_t a = {0};
        a.type = MAC_TYPE_COMMAND; a.ack_request = true; a.seq = 7;
        a.dst_mode = MAC_ADDR_SHORT; a.dst_pan = PAN; a.dst_short = 0;
        a.src_mode = MAC_ADDR_EXT; a.src_pan = 0xFFFF; a.src_ext = DEV_EXT;
        const uint8_t p[2] = {MAC_CMD_ASSOC_REQUEST, 0x8E};
        uint8_t buf[64];
        size_t n = mac_frame_build(&a, p, 2, buf, sizeof buf);
        mac_rx(&mac, buf, (uint8_t)n, 180, -60);
        CHECK(assoc_ind == 1 && last_assoc == DEV_EXT && last_cap == 0x8E);
    }
    unsigned before = sent_count;
    CHECK(mac_association_response(&mac, DEV_EXT, 0x4E21, 0) == MAC_STATUS_SUCCESS);
    CHECK(sent_count == before);                                        /* indirect: waits for poll */
    CHECK(sm[sm_count - 1].ext && sm[sm_count - 1].addr == DEV_EXT && sm[sm_count - 1].pending);
    poll_ext(DEV_EXT);
    CHECK(sent_count == before + 1);
    f = last_sent();
    CHECK(f.type == MAC_TYPE_COMMAND && f.payload[0] == MAC_CMD_ASSOC_RESPONSE && f.dst_ext == DEV_EXT);
    CHECK(f.src_ext == COORD_EXT && f.payload[1] == 0x21 && f.payload[2] == 0x4E && !f.pending);
    mac_tx_done(&mac, MAC_TX_OK, false);
    CHECK(comm_count == 1 && last_comm_dev == DEV_EXT && last_comm_status == MAC_STATUS_SUCCESS);
    CHECK(!sm[sm_count - 1].pending && sm[sm_count - 1].addr == DEV_EXT);

    /* Direct unicast: MAC_TX_ROUNDS rounds of 1 + 3 retries on NO_ACK (same
     * DSN), the frame going back to the end of the queue between rounds,
     * then the NO_ACK confirm. */
    before = sent_count;
    CHECK(data(0x4E21, 10, false) == MAC_STATUS_SUCCESS);
    uint8_t dsn = sent[sent_count - 1].mpdu[2];
    const unsigned attempts = MAC_TX_ROUNDS * (1u + MAC_MAX_FRAME_RETRIES);
    for (unsigned i = 0; i < attempts; i++) {
        CHECK(sent_count == before + 1 + i && sent[sent_count - 1].ack && sent[sent_count - 1].mpdu[2] == dsn);
        mac_tx_done(&mac, MAC_TX_NO_ACK, false);
    }
    CHECK(sent_count == before + attempts && confirm_count == 1 && confirms[0][0] == 10 &&
          confirms[0][1] == MAC_STATUS_NO_ACK);
    CHECK(mac.stats.tx_retries == MAC_TX_ROUNDS * MAC_MAX_FRAME_RETRIES && mac.stats.tx_rounds == MAC_TX_ROUNDS - 1);
    /* Success after one retry. */
    CHECK(data(0x4E21, 11, false) == MAC_STATUS_SUCCESS);
    mac_tx_done(&mac, MAC_TX_NO_ACK, false);
    mac_tx_done(&mac, MAC_TX_OK, false);
    CHECK(confirm_count == 2 && confirms[1][0] == 11 && confirms[1][1] == MAC_STATUS_SUCCESS);
    /* CSMA failure: another round at the END of the queue (a frame queued
     * meanwhile goes first), CHANNEL_ACCESS_FAILURE after MAC_TX_ROUNDS. */
    before = sent_count;
    CHECK(data(0x4E21, 12, false) == MAC_STATUS_SUCCESS);
    CHECK(data(0x4E22, 15, false) == MAC_STATUS_SUCCESS);   /* waits behind 12 */
    mac_tx_done(&mac, MAC_TX_BUSY, false);                  /* 12 busy: re-queued after 15 */
    CHECK(sent_count == before + 2 && last_sent().dst_short == 0x4E22);
    mac_tx_done(&mac, MAC_TX_OK, false);                    /* 15 sent */
    CHECK(confirm_count == 3 && confirms[2][0] == 15 && confirms[2][1] == MAC_STATUS_SUCCESS);
    for (unsigned r = 1; r < MAC_TX_ROUNDS; r++) {
        CHECK(last_sent().dst_short == 0x4E21);
        mac_tx_done(&mac, MAC_TX_BUSY, false);
    }
    CHECK(sent_count == before + 1 + MAC_TX_ROUNDS && confirm_count == 4 && confirms[3][0] == 12 &&
          confirms[3][1] == MAC_STATUS_CHANNEL_ACCESS_FAILURE);
    /* Broadcast: no ACK request. */
    CHECK(data(0xFFFF, 13, false) == MAC_STATUS_SUCCESS && !sent[sent_count - 1].ack);
    mac_tx_done(&mac, MAC_TX_OK, false);
    CHECK(data(0xFFFF, 14, true) == MAC_STATUS_INVALID_PARAMETER);
    /* Queueing: frames wait while one is in flight, order preserved. */
    CHECK(data(0x0001, 20, false) == 0 && data(0x0002, 21, false) == 0 && data(0x0003, 22, false) == 0);
    for (unsigned i = 0; i < 3; i++) {
        f = last_sent();
        CHECK(f.dst_short == i + 1);
        mac_tx_done(&mac, MAC_TX_OK, false);
    }
    /* Radio refuses to start: confirm with a local radio error. */
    tx_fail = 1;
    CHECK(data(0x0001, 23, false) == 0);
    tx_fail = 0;
    CHECK(confirms[(confirm_count - 1) % 64][0] == 23 && confirms[(confirm_count - 1) % 64][1] == MAC_STATUS_RADIO_ERROR);

    /* Indirect to a sleepy child: quota of 3, frame-pending bit, source match. */
    unsigned confirms_before = confirm_count, sm_before = sm_count;
    CHECK(data(0x5555, 30, true) == 0 && data(0x5555, 31, true) == 0 && data(0x5555, 32, true) == 0);
    CHECK(data(0x5555, 33, true) == MAC_STATUS_TRANSACTION_OVERFLOW);
    CHECK(sm_count == sm_before + 1 && sm[sm_before].addr == 0x5555 && sm[sm_before].pending);
    CHECK(mac_pending_count(&mac, 0x5555) == 3);
    before = sent_count;
    poll_short(0x5555);
    CHECK(sent_count == before + 1);
    f = last_sent();
    CHECK(f.dst_short == 0x5555 && f.pending && f.payload[0] == 30);
    mac_tx_done(&mac, MAC_TX_NO_ACK, false);                         /* stays queued */
    CHECK(mac_pending_count(&mac, 0x5555) == 3 && confirm_count == confirms_before);
    poll_short(0x5555);
    f = last_sent();
    CHECK(f.payload[0] == 30 && f.seq == sent[before].mpdu[2]);      /* same frame, same DSN */
    mac_tx_done(&mac, MAC_TX_OK, false);
    poll_short(0x5555);
    f = last_sent();
    CHECK(f.payload[0] == 31 && f.pending);
    mac_tx_done(&mac, MAC_TX_OK, false);
    poll_short(0x5555);
    f = last_sent();
    CHECK(f.payload[0] == 32 && !f.pending);
    mac_tx_done(&mac, MAC_TX_OK, false);
    CHECK(confirm_count == confirms_before + 3 && confirms[(confirm_count - 1) % 64][0] == 32);
    CHECK(!sm[sm_count - 1].pending && sm[sm_count - 1].addr == 0x5555);
    before = sent_count;
    poll_short(0x5555);
    CHECK(sent_count == before && mac.stats.polls_empty == 1);

    /* Expiry after macTransactionPersistenceTime. */
    mac_tick(&mac, 2000);
    CHECK(data(0x6666, 40, true) == 0);
    mac_tick(&mac, 2000 + MAC_PERSISTENCE_MS - 1);
    CHECK(mac_pending_count(&mac, 0x6666) == 1);
    mac_tick(&mac, 2000 + MAC_PERSISTENCE_MS);
    CHECK(mac_pending_count(&mac, 0x6666) == 0);
    CHECK(confirms[(confirm_count - 1) % 64][0] == 40 && confirms[(confirm_count - 1) % 64][1] == MAC_STATUS_TRANSACTION_EXPIRED);
    CHECK(!sm[sm_count - 1].pending);
    /* Purge: no confirm. */
    CHECK(data(0x7777, 41, true) == 0);
    unsigned c = confirm_count;
    CHECK(mac_purge(&mac, 41) && !mac_purge(&mac, 41) && confirm_count == c);
    /* Indirect saturation is reported, never overwritten, and never takes
     * the buffers reserved for direct traffic. */
    unsigned accepted = 0, full_before = mac.stats.queue_full;
    for (uint16_t a = 0x100; a < 0x100 + 30; a++) if (data(a, 50, true) == 0) accepted++;
    CHECK(accepted == MAC_INDIRECT_SLOTS && mac.stats.queue_full == full_before + 30 - MAC_INDIRECT_SLOTS);
    unsigned free_slots = 0;
    for (unsigned i = 0; i < MAC_POOL_SLOTS; i++) free_slots += mac.slots[i].kind == SLOT_FREE;
    CHECK(free_slots >= MAC_POOL_SLOTS - MAC_INDIRECT_SLOTS - 1);   /* direct traffic still has buffers */
    mac_tick(&mac, 2000 + 3 * MAC_PERSISTENCE_MS);
    CHECK(data(0x0200, 51, true) == 0);
    mac_tick(&mac, 2000 + 5 * MAC_PERSISTENCE_MS);

    /* Data indication, filtering and duplicate rejection. */
    const uint8_t zcl[4] = {1, 2, 3, 4};
    unsigned ind = indications;
    rx_from_short(0x4E21, PAN, 0x0000, zcl, 4, MAC_TYPE_DATA, false);
    CHECK(indications == ind + 1 && last_payload_len == 4 && last_payload[3] == 4);
    rx_from_short(0x4E21, PAN, 0x0000, zcl, 4, MAC_TYPE_DATA, true);   /* MAC retransmission */
    CHECK(indications == ind + 1 && mac.stats.rx_duplicates == 1);
    rx_from_short(0x4E21, PAN, 0x0000, zcl, 4, MAC_TYPE_DATA, false);
    CHECK(indications == ind + 2);
    rx_from_short(0x4E21, PAN + 1, 0x0000, zcl, 4, MAC_TYPE_DATA, false); /* other PAN */
    rx_from_short(0x4E21, PAN, 0x0007, zcl, 4, MAC_TYPE_DATA, false);     /* other node */
    CHECK(indications == ind + 2 && mac.stats.rx_filtered >= 2);
    rx_from_short(0x4E21, PAN, 0xFFFF, zcl, 4, MAC_TYPE_DATA, false);     /* broadcast */
    CHECK(indications == ind + 3);

    /* Orphan notification -> indication; realignment frame content. */
    {
        mac_frame_t o = {0};
        o.type = MAC_TYPE_COMMAND; o.pan_id_compression = true; o.seq = 3;
        o.dst_mode = MAC_ADDR_SHORT; o.dst_pan = 0xFFFF; o.dst_short = 0xFFFF;
        o.src_mode = MAC_ADDR_EXT; o.src_ext = DEV_EXT;
        const uint8_t p = MAC_CMD_ORPHAN_NOTIFY;
        uint8_t buf[64];
        size_t n = mac_frame_build(&o, &p, 1, buf, sizeof buf);
        mac_rx(&mac, buf, (uint8_t)n, 100, -70);
        CHECK(orphan_ind == 1);
        CHECK(mac_orphan_response(&mac, DEV_EXT, 0x4E21) == 0);
        f = last_sent();
        CHECK(f.payload[0] == MAC_CMD_COORD_REALIGN && f.dst_pan == 0xFFFF && f.src_pan == PAN);
        CHECK(f.payload[5] == 15 && f.payload[6] == 0x21 && f.payload[7] == 0x4E);
        mac_tx_done(&mac, MAC_TX_OK, false);
        CHECK(last_comm_status == MAC_STATUS_SUCCESS && last_comm_dev == DEV_EXT);
    }

    /* Randomised stress: arbitrary frames, results and time never break the
     * invariants (one TX at a time, bounded pool). */
    uint32_t now = 100000;
    for (unsigned i = 0; i < 200000; i++) {
        unsigned op = frt_rand() % 6;
        if (op == 0) {
            uint8_t buf[MAC_MAX_MPDU];
            uint8_t len = (uint8_t)(frt_rand() % 40);
            for (unsigned k = 0; k < len; k++) buf[k] = (uint8_t)frt_rand();
            mac_rx(&mac, buf, len, 1, -1);
        } else if (op == 1) {
            (void)data((uint16_t)(frt_rand() % 8), (uint8_t)i, frt_rand() & 1);
        } else if (op == 2) {
            poll_short((uint16_t)(frt_rand() % 8));
        } else if (op == 3 && mac.active >= 0) {
            mac_tx_done(&mac, (mac_tx_result_t)(frt_rand() % 4), false);
        } else if (op == 4) {
            now += frt_rand() % 3000;
            mac_tick(&mac, now);
        } else {
            sent_count = 0;
        }
        unsigned used = 0;
        for (unsigned k = 0; k < MAC_POOL_SLOTS; k++) used += mac.slots[k].kind != SLOT_FREE;
        CHECK(used <= MAC_POOL_SLOTS && mac.indirect.count <= CORE_INDIRECT_CAPACITY);
        CHECK(mac.active < (int)MAC_POOL_SLOTS);
        if (mac.active >= 0) CHECK(mac.slots[mac.active].kind != SLOT_FREE && mac.slots[mac.active].in_flight);
    }
    /* Drain: everything eventually completes or expires. */
    for (unsigned i = 0; i < 1000; i++) {
        if (mac.active >= 0) mac_tx_done(&mac, MAC_TX_OK, false);
        now += 1000;
        mac_tick(&mac, now);
    }
    unsigned used = 0;
    for (unsigned k = 0; k < MAC_POOL_SLOTS; k++) used += mac.slots[k].kind != SLOT_FREE;
    CHECK(used == 0 && mac.indirect.count == 0 && mac.active < 0);
    PASS("mac_core");
    return 0;
}
