/* SPDX-License-Identifier: MIT
 * End-to-end join between the coordinator engine (zb_coord) and a simulated
 * Zigbee 3.0 device built only from the frame codecs, over a simulated radio.
 */
#include "frt_test.h"
#include "aps_security.h"
#include "trust_center.h"
#include "zb_ccm_backend.h"
#include "zb_coord.h"
#include "zb_mmo.h"
#include "nv_log.h"

/* RAM-backed NOR for the coordinator store (2 banks x 2 sectors of 4 KiB). */
#define STORE_SECTOR 4096u
#define STORE_BANK (2u * STORE_SECTOR)
static uint8_t store_mem[2u * STORE_BANK];
static bool s_read(void *ctx, uint32_t a, void *b, size_t n) { (void)ctx; memcpy(b, store_mem + a, n); return true; }
static bool s_prog(void *ctx, uint32_t a, const void *d, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n; i++) store_mem[a + i] &= ((const uint8_t *)d)[i];
    return true;
}
static bool s_erase(void *ctx, uint32_t a) { (void)ctx; memset(store_mem + a, 0xFF, STORE_SECTOR); return true; }
static const nv_flash_ops_t STORE_OPS = {s_read, s_prog, s_erase, 0};
static nv_log_t store;

#define COORD_EXT 0x00124B0001020304ull
#define PAN 0x1A62

/* ---- simulated radio: the coordinator's transmissions land here ------- */
static uint8_t air[32][MAC_MAX_MPDU];
static uint8_t air_len[32];
static unsigned air_count, air_read;
static int sim_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool ack) {
    (void)ctx; (void)ack;
    unsigned i = air_count++ % 32;
    memcpy(air[i], mpdu, len);
    air_len[i] = len;
    return 0;
}
static void sim_match(void *ctx, bool ext, uint64_t a, bool p) { (void)ctx; (void)ext; (void)a; (void)p; }

static unsigned joined_events, aps_events;
static uint16_t joined_short;
static uint64_t joined_ext;
static uint32_t reserved;
static void up_joined(void *ctx, uint16_t s, uint64_t e, uint8_t cap) {
    (void)ctx; (void)cap; joined_events++; joined_short = s; joined_ext = e;
}
static void up_aps(void *ctx, const nwk_header_t *n, const aps_header_t *a, const uint8_t *p, size_t len, uint8_t lqi) {
    (void)ctx; (void)n; (void)p; (void)lqi;
    if (a->cluster == 0x0006 && len == 4) aps_events++;
}
static bool up_reserve(void *ctx, uint32_t upper) { (void)ctx; reserved = upper; return true; }
static unsigned key_counter, keys_established;
static bool up_random(void *ctx, uint8_t key[16]) {
    (void)ctx;
    key_counter++;
    for (unsigned i = 0; i < 16; i++) key[i] = (uint8_t)(0x5A ^ (i * 17) ^ key_counter);
    return true;
}
static void up_key(void *ctx, uint16_t s, uint64_t e) { (void)ctx; (void)s; (void)e; keys_established++; }

static zb_coord_t coord;
static const mac_radio_ops_t RADIO = {sim_tx, sim_match, 0};
static const zb_coord_upper_t UPPER = {up_joined, up_aps, up_reserve, up_random, up_key, 0, 0, 0, 0, 0};

/* ---- simulated device ----------------------------------------------- */
typedef struct {
    uint64_t ext;
    uint8_t tclk[16], capability, mac_seq, nwk_seq, aps_counter;
    uint16_t short_addr;
    uint8_t network_key[16];
    bool has_key;
    uint32_t aps_fc;
    core_security_t sec;
} device_t;

static bool dev_reserve(void *ctx, uint32_t upper) { (void)ctx; (void)upper; return true; }

static void dev_init(device_t *d, uint64_t ext, const uint8_t tclk[16], uint8_t capability) {
    memset(d, 0, sizeof *d);
    d->ext = ext;
    memcpy(d->tclk, tclk, 16);
    d->capability = capability;
    d->short_addr = 0xFFFF;
    core_security_init(&d->sec, 1, zb_ccm_backend_sw, dev_reserve, 0);
}

static void dev_mac_cmd_ext(device_t *d, uint16_t src_pan, const uint8_t *payload, uint8_t len) {
    mac_frame_t f = {0};
    f.type = MAC_TYPE_COMMAND; f.ack_request = true; f.seq = d->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0x0000;
    f.src_mode = MAC_ADDR_EXT; f.src_ext = d->ext;
    f.src_pan = src_pan; f.pan_id_compression = src_pan == PAN;
    uint8_t buf[MAC_MAX_MPDU];
    size_t n = mac_frame_build(&f, payload, len, buf, sizeof buf);
    CHECK(n);
    zb_coord_rx(&coord, buf, (uint8_t)n, 200, -50);
}
static void dev_associate(device_t *d) {
    const uint8_t p[2] = {MAC_CMD_ASSOC_REQUEST, d->capability};
    dev_mac_cmd_ext(d, 0xFFFF, p, 2);
}
static void dev_poll(device_t *d) {
    const uint8_t p = MAC_CMD_DATA_REQUEST;
    if (d->short_addr == 0xFFFF) { dev_mac_cmd_ext(d, PAN, &p, 1); return; }
    mac_frame_t f = {0};
    f.type = MAC_TYPE_COMMAND; f.ack_request = true; f.pan_id_compression = true; f.seq = d->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = d->short_addr;
    uint8_t buf[32];
    size_t n = mac_frame_build(&f, &p, 1, buf, sizeof buf);
    zb_coord_rx(&coord, buf, (uint8_t)n, 200, -50);
}

/* Receive the coordinator's last transmission and acknowledge it. */
static bool dev_receive(device_t *d) {
    CHECK(air_read < air_count);
    const uint8_t *mpdu = air[air_read % 32];
    mac_frame_t f;
    CHECK(mac_frame_parse(mpdu, air_len[air_read % 32], &f) == MAC_OK);
    air_read++;
    zb_coord_tx_done(&coord, MAC_TX_OK, false);
    if (f.type == MAC_TYPE_COMMAND && f.payload[0] == MAC_CMD_ASSOC_RESPONSE) {
        CHECK(f.dst_ext == d->ext);
        if (f.payload[3] == 0) d->short_addr = (uint16_t)(f.payload[1] | (f.payload[2] << 8));
        return f.payload[3] == 0;
    }
    if (f.type != MAC_TYPE_DATA || f.dst_short != d->short_addr) return false;
    uint8_t frame[MAC_MAX_MPDU];
    memcpy(frame, f.payload, f.payload_len);
    nwk_header_t h;
    aps_header_t a;
    if (nwk_parse(frame, f.payload_len, &h) != NWK_OK || h.security) return false;
    if (aps_parse(h.payload, h.payload_len, &a) != APS_OK || a.type != APS_TYPE_COMMAND) return false;
    uint8_t ktk[16];
    zb_key_transport_key(d->tclk, ktk);
    nwk_aux_t aux;
    size_t off, len;
    uint8_t *aps = frame + h.header_len;
    if (!aps_open(ktk, aps, h.payload_len, a.header_len, &aux, &off, &len)) return false;
    if (len != 35 || aps[off] != APS_CMD_TRANSPORT_KEY || aux.source != COORD_EXT) return false;
    memcpy(d->network_key, aps + off + 2, 16);
    d->has_key = core_security_set_key(&d->sec, 0, aps[off + 18], d->network_key);
    return d->has_key;
}

/* Next coordinator transmission, NWK-secured: decrypt NWK and APS. */
typedef struct { aps_header_t a; uint8_t payload[100]; size_t len; uint8_t aps_key_id; bool aps_secured; } dev_rx_t;
static bool dev_take(device_t *d, dev_rx_t *out) {
    CHECK(air_read < air_count);
    const uint8_t *mpdu = air[air_read % 32];
    mac_frame_t f;
    CHECK(mac_frame_parse(mpdu, air_len[air_read % 32], &f) == MAC_OK);
    air_read++;
    zb_coord_tx_done(&coord, MAC_TX_OK, false);
    if (f.type != MAC_TYPE_DATA || f.dst_short != d->short_addr) return false;
    uint8_t frame[MAC_MAX_MPDU];
    memcpy(frame, f.payload, f.payload_len);
    nwk_header_t h;
    if (nwk_parse(frame, f.payload_len, &h) != NWK_OK || !h.security) return false;
    size_t plen;
    if (!core_security_open_authenticated_peer(&d->sec, frame, f.payload_len, h.header_len, &plen)) return false;
    uint8_t *aps = frame + h.header_len + 14;
    if (aps_parse(aps, plen, &out->a) != APS_OK) return false;
    out->aps_secured = out->a.security;
    if (out->a.security) {
        nwk_aux_t aux;
        if (nwk_aux_parse(aps + out->a.header_len, plen - out->a.header_len, &aux) != NWK_OK) return false;
        uint8_t key[16];
        if (aux.key_id == APS_KEY_TRANSPORT) zb_key_transport_key(d->tclk, key);
        else memcpy(key, d->tclk, 16);
        size_t off, len;
        if (!aps_open(key, aps, plen, out->a.header_len, &aux, &off, &len)) return false;
        out->aps_key_id = aux.key_id;
        memcpy(out->payload, aps + off, len);
        out->len = len;
    } else {
        memcpy(out->payload, out->a.payload, out->a.payload_len);
        out->len = out->a.payload_len;
    }
    return true;
}

/* Build a NWK-secured data frame from the device and deliver it. */
static size_t dev_build_secured(device_t *d, uint16_t nwk_dst, const uint8_t *aps, size_t aps_len,
                                uint64_t claimed_ext, uint8_t *mpdu) {
    nwk_header_t h = {0};
    h.type = NWK_TYPE_DATA; h.dst = nwk_dst; h.src = d->short_addr; h.radius = 30; h.seq = d->nwk_seq++;
    h.has_src_ieee = true; h.src_ieee = claimed_ext;
    uint8_t nwk[CORE_SECURITY_MAX_FRAME];
    size_t hl = nwk_build(&h, nwk, sizeof nwk);
    memcpy(nwk + hl, aps, aps_len);
    uint8_t local[8];
    for (unsigned i = 0; i < 8; i++) local[i] = (uint8_t)(claimed_ext >> (8 * i));
    size_t total = 0;
    CHECK(core_security_seal(&d->sec, 0, local, nwk, hl, aps_len, sizeof nwk, &total));
    mac_frame_t f = {0};
    f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.seq = d->mac_seq++;
    f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = nwk_dst >= 0xFFF8 ? 0xFFFF : 0x0000;
    f.ack_request = nwk_dst < 0xFFF8;
    f.src_mode = MAC_ADDR_SHORT; f.src_short = d->short_addr;
    return mac_frame_build(&f, nwk, total, mpdu, MAC_MAX_MPDU);
}

static size_t dev_announce(device_t *d, uint64_t claimed_ext, uint8_t *mpdu) {
    uint8_t aps[8 + 12] = {0x08 /* data, broadcast */, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, d->aps_counter++};
    uint8_t *z = aps + 8;
    z[0] = 0x55;
    z[1] = (uint8_t)d->short_addr; z[2] = (uint8_t)(d->short_addr >> 8);
    for (unsigned i = 0; i < 8; i++) z[3 + i] = (uint8_t)(claimed_ext >> (8 * i));
    z[11] = d->capability;
    return dev_build_secured(d, NWK_BCAST_RX_ON, aps, sizeof aps, claimed_ext, mpdu);
}

static void dev_send_nwk(device_t *d, const uint8_t *aps, size_t len) {
    uint8_t mpdu[MAC_MAX_MPDU];
    size_t n = dev_build_secured(d, 0x0000, aps, len, d->ext, mpdu);
    CHECK(n);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -55);
}
static void dev_aps_command(device_t *d, const uint8_t *cmd, size_t len, bool secured) {
    uint8_t hdr[2] = {0x01, d->aps_counter++};
    uint8_t aps[MAC_MAX_PAYLOAD];
    size_t n;
    if (secured) {
        nwk_aux_t aux = {.key_id = APS_KEY_DATA, .extended_nonce = true, .frame_counter = d->aps_fc++, .source = d->ext};
        n = aps_seal(d->tclk, hdr, 2, &aux, cmd, len, aps, sizeof aps);
    } else {
        memcpy(aps, hdr, 2);
        memcpy(aps + 2, cmd, len);
        n = 2 + len;
    }
    CHECK(n);
    dev_send_nwk(d, aps, n);
}
static void dev_zdo_request(device_t *d, uint16_t cluster, const uint8_t *p, size_t len, bool ack) {
    aps_header_t a = {0};
    a.type = APS_TYPE_DATA; a.ack_request = ack; a.cluster = cluster; a.counter = d->aps_counter++;
    uint8_t aps[MAC_MAX_PAYLOAD];
    size_t hl = aps_build(&a, aps, sizeof aps);
    memcpy(aps + hl, p, len);
    dev_send_nwk(d, aps, hl + len);
}

static void join(device_t *d, bool expect_ok) {
    unsigned before = joined_events;
    dev_associate(d);
    dev_poll(d);                        /* association response is indirect */
    bool admitted = dev_receive(d);
    CHECK(admitted == expect_ok);
    if (!admitted) return;
    if (!(d->capability & NWK_CAP_RX_ON_IDLE)) dev_poll(d);   /* sleepy: key is indirect too */
    CHECK(dev_receive(d));
    uint8_t mpdu[MAC_MAX_MPDU];
    size_t n = dev_announce(d, d->ext, mpdu);
    CHECK(n);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 180, -60);
    CHECK(joined_events == before + 1 && joined_ext == d->ext && joined_short == d->short_addr);
}

int main(void) {
    static device_t router, sleepy, intruder, coded, coded_wrong;
    const uint8_t epid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t nwk_key[16];
    for (unsigned i = 0; i < 16; i++) nwk_key[i] = (uint8_t)(0xA0 + i);
    memset(store_mem, 0xFF, sizeof store_mem);
    CHECK(nv_log_mount(&store, &STORE_OPS, 0, STORE_BANK, STORE_SECTOR));
    zb_coord_init(&coord, &RADIO, &UPPER, COORD_EXT, 0x1234ABCDu, 1000);
    zb_coord_attach_store(&coord, &store);
    CHECK(!zb_coord_restore(&coord));                       /* empty store: nothing to restore */
    zb_coord_form(&coord, PAN, 15, epid, nwk_key, 0);

    /* Closed network: refused. */
    dev_init(&intruder, 0x1111111111111111ull, TC_DEFAULT_LINK_KEY, 0x8E);
    join(&intruder, false);
    CHECK(coord.stats.associations_denied == 1 && coord.devices.count == 0);

    /* Router-like device (RX on when idle): direct Transport Key. */
    zb_coord_permit_join(&coord, 60);
    dev_init(&router, 0xA4C1380000000001ull, TC_DEFAULT_LINK_KEY, 0x8E);
    join(&router, true);
    CHECK(!memcmp(router.network_key, nwk_key, 16));
    CHECK(coord.stats.transport_keys_sent == 1 && coord.stats.joined == 1);
    /* Sleepy end device: association response and key delivered on polls. */
    dev_init(&sleepy, 0xA4C1380000000002ull, TC_DEFAULT_LINK_KEY, 0x80);
    join(&sleepy, true);
    CHECK(sleepy.short_addr != router.short_addr && coord.stats.joined == 2);

    /* Install-code device: the coordinator uses the derived key. */
    const uint8_t code[18] = {0x83, 0xFE, 0xD3, 0x40, 0x7A, 0x93, 0x97, 0x23, 0xA5, 0xC6,
                              0x39, 0xB2, 0x69, 0x16, 0xD5, 0x05, 0xC3, 0xB5};
    uint8_t ic_key[16];
    CHECK(zb_install_code_to_key(code, 18, ic_key));
    CHECK(zb_coord_set_link_key(&coord, 0xA4C1380000000003ull, ic_key));
    dev_init(&coded, 0xA4C1380000000003ull, ic_key, 0x8E);
    join(&coded, true);
    /* Same IEEE but the device only knows the default key: it cannot read
     * the network key (it remains unauthorized on the coordinator). */
    CHECK(zb_coord_set_link_key(&coord, 0xA4C1380000000004ull, ic_key));
    dev_init(&coded_wrong, 0xA4C1380000000004ull, TC_DEFAULT_LINK_KEY, 0x8E);
    dev_associate(&coded_wrong);
    dev_poll(&coded_wrong);
    CHECK(dev_receive(&coded_wrong));
    CHECK(!dev_receive(&coded_wrong) && !coded_wrong.has_key);
    CHECK(!coord.devices.dev[nwk_devices_find_ext(&coord.devices, coded_wrong.ext)].authorized);

    /* Default TCLK disallowed and no install code: refused at association. */
    coord.allow_default_tclk = false;
    dev_init(&intruder, 0x2222222222222222ull, TC_DEFAULT_LINK_KEY, 0x8E);
    join(&intruder, false);
    coord.allow_default_tclk = true;

    /* Replay of a valid announce is rejected; tampering is rejected. */
    uint8_t mpdu[MAC_MAX_MPDU];
    size_t n = dev_announce(&router, router.ext, mpdu);
    unsigned joined_before = joined_events;
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 180, -60);
    CHECK(joined_events == joined_before + 1);           /* fresh counter: accepted (re-announce) */
    uint32_t auth_fail = coord.stats.nwk_auth_failures;
    /* Exact replay. A broadcast carries no MAC ACK request, so the MAC does
     * not de-duplicate it: the NWK frame-counter check must reject it. */
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 180, -60);
    CHECK(coord.stats.nwk_auth_failures == auth_fail + 1 && joined_events == joined_before + 1);
    n = dev_announce(&router, router.ext, mpdu);
    mpdu[n - 1] ^= 0x01;                                  /* MIC altered */
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 180, -60);
    CHECK(coord.stats.nwk_auth_failures == auth_fail + 2 && joined_events == joined_before + 1);

    /* A node unknown to the Trust Center but holding the network key (e.g. a
     * device herdsman's restore could not carry) is a network member: its
     * authenticated frame is accepted and the node learnt (Z-Stack does the
     * same). Without the key, nothing is learnt (see the MIC checks above). */
    device_t ghost;
    dev_init(&ghost, 0x3333333333333333ull, TC_DEFAULT_LINK_KEY, 0x8E);
    ghost.short_addr = 0x7777;
    CHECK(core_security_set_key(&ghost.sec, 0, 0, nwk_key));
    uint32_t unknown = coord.stats.nwk_unknown_source;
    n = dev_announce(&ghost, ghost.ext, mpdu);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 180, -60);
    CHECK(coord.stats.nwk_unknown_source == unknown + 1 && coord.stats.learned_members == 1);
    CHECK(nwk_devices_find_ext(&coord.devices, ghost.ext) != NWK_DEVICE_NONE);

    /* Application data (ZCL On/Off report) goes up to the host side. */
    const uint8_t zcl[8 + 4] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, router.aps_counter++,
                                0x18, 0x01, 0x0A, 0x00};
    n = dev_build_secured(&router, 0x0000, zcl, sizeof zcl, router.ext, mpdu);
    zb_coord_rx(&coord, mpdu, (uint8_t)n, 180, -60);
    CHECK(aps_events == 1 && coord.stats.aps_data_up == 1);
    zb_coord_tx_done(&coord, MAC_TX_OK, false);
    /* Unsecured NWK data after joining is dropped. */
    {
        nwk_header_t h = {0};
        h.type = NWK_TYPE_DATA; h.dst = 0; h.src = router.short_addr; h.radius = 1;
        uint8_t nwk[32];
        size_t hl = nwk_build(&h, nwk, sizeof nwk);
        memcpy(nwk + hl, zcl, sizeof zcl);
        mac_frame_t f = {0};
        f.type = MAC_TYPE_DATA; f.pan_id_compression = true; f.seq = 99;
        f.dst_mode = MAC_ADDR_SHORT; f.dst_pan = PAN; f.dst_short = 0;
        f.src_mode = MAC_ADDR_SHORT; f.src_short = router.short_addr;
        n = mac_frame_build(&f, nwk, hl + sizeof zcl, mpdu, sizeof mpdu);
        zb_coord_rx(&coord, mpdu, (uint8_t)n, 180, -60);
        CHECK(coord.stats.nwk_unsecured_dropped == 1 && aps_events == 1);
    }

    /* Zigbee 3.0 unique TC link key: Request -> Transport -> Verify -> Confirm. */
    {
        CHECK(air_read == air_count);
        const uint8_t req[2] = {0x08, 0x04};
        dev_aps_command(&router, req, 2, true);
        CHECK(coord.stats.key_requests == 1 && coord.stats.link_keys_sent == 1);
        dev_rx_t rx;
        CHECK(dev_take(&router, &rx));
        CHECK(rx.aps_secured && rx.aps_key_id == APS_KEY_TRANSPORT && rx.a.type == APS_TYPE_COMMAND);
        CHECK(rx.len == 34 && rx.payload[0] == 0x05 && rx.payload[1] == 0x04);
        uint8_t new_key[16];
        memcpy(new_key, rx.payload + 2, 16);
        CHECK(memcmp(new_key, TC_DEFAULT_LINK_KEY, 16) != 0);
        /* A wrong verification hash is refused and nothing is confirmed. */
        uint8_t verify[26] = {0x0F, 0x04};
        for (unsigned i = 0; i < 8; i++) verify[2 + i] = (uint8_t)(router.ext >> (8 * i));
        memset(verify + 10, 0x77, 16);
        dev_aps_command(&router, verify, sizeof verify, false);
        CHECK(coord.stats.key_verify_failures == 1 && air_read == air_count && keys_established == 0);
        const uint8_t three = 0x03;
        CHECK(zb_hmac_mmo(new_key, &three, 1, verify + 10));
        dev_aps_command(&router, verify, sizeof verify, false);
        CHECK(coord.stats.keys_verified == 1 && keys_established == 1);
        memcpy(router.tclk, new_key, 16);              /* device switches to the unique key */
        router.aps_fc = 0;
        CHECK(dev_take(&router, &rx));
        CHECK(rx.aps_secured && rx.aps_key_id == APS_KEY_DATA && rx.len == 11 && rx.payload[0] == 0x10);
        CHECK(rx.payload[1] == 0x00 && rx.payload[2] == 0x04);
        uint16_t slot = nwk_devices_find_ext(&coord.devices, router.ext);
        CHECK(coord.devices.dev[slot].unique_key && !memcmp(coord.devices.dev[slot].link_key, new_key, 16));
        /* APS-secured data with the unique key is accepted, a replay is not. */
        uint8_t hdr[8] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, router.aps_counter++};
        const uint8_t report[4] = {0x18, 0x02, 0x0A, 0x00};
        nwk_aux_t aux = {.key_id = APS_KEY_DATA, .extended_nonce = true, .frame_counter = router.aps_fc++, .source = router.ext};
        uint8_t aps[64];
        size_t an = aps_seal(router.tclk, hdr, sizeof hdr, &aux, report, sizeof report, aps, sizeof aps);
        unsigned before = aps_events;
        dev_send_nwk(&router, aps, an);
        CHECK(aps_events == before + 1);
        dev_send_nwk(&router, aps, an);                 /* new NWK counter, same APS counter */
        CHECK(aps_events == before + 1 && coord.stats.aps_replays == 1);
    }

    /* ZDO server: Node Descriptor request with APS ACK requested. */
    {
        const uint8_t nd[3] = {0x31, 0x00, 0x00};
        dev_zdo_request(&router, 0x0002, nd, 3, true);
        dev_rx_t ack, rsp;
        CHECK(dev_take(&router, &ack) && ack.a.type == APS_TYPE_ACK && ack.a.cluster == 0x0002);
        CHECK(dev_take(&router, &rsp) && rsp.a.cluster == 0x8002 && rsp.a.profile == 0);
        CHECK(rsp.len == 1 + 1 + 2 + 13 && rsp.payload[0] == 0x31 && rsp.payload[1] == 0 && rsp.payload[4] == 0x00);
        CHECK(rsp.payload[6] == 0x8F && rsp.payload[12] == 0x41 && rsp.payload[13] == 0x2C); /* mask after max buf + max in */
        const uint8_t ae[3] = {0x32, 0x00, 0x00};
        dev_zdo_request(&router, 0x0005, ae, 3, false);
        CHECK(dev_take(&router, &rsp) && rsp.a.cluster == 0x8005 && rsp.len == 6 && rsp.payload[5] == 1);
        const uint8_t sd[4] = {0x33, 0x00, 0x00, 0x01};
        dev_zdo_request(&router, 0x0004, sd, 4, false);
        CHECK(dev_take(&router, &rsp) && rsp.a.cluster == 0x8004 && rsp.payload[1] == 0 && rsp.payload[5] == 1);
        /* Match Descriptor for OTA client -> our OTA server cluster matches. */
        const uint8_t md[9] = {0x34, 0x00, 0x00, 0x04, 0x01, 0x01, 0x19, 0x00, 0x00};
        dev_zdo_request(&router, 0x0006, md, 7, false);        /* truncated: ignored, no reply */
        CHECK(air_read == air_count);
        dev_zdo_request(&router, 0x0006, md, 9, false);
        CHECK(dev_take(&router, &rsp) && rsp.a.cluster == 0x8006 && rsp.payload[4] == 1 && rsp.payload[5] == 1);
        const uint8_t bind[2] = {0x35, 0x00};
        dev_zdo_request(&router, 0x0021, bind, 2, false);
        CHECK(dev_take(&router, &rsp) && rsp.a.cluster == 0x8021 && rsp.payload[1] == 0x84);
        CHECK(coord.stats.zdo_requests == 6 && coord.stats.zdo_responses_sent == 5 && coord.stats.aps_acks_sent == 1);
    }

    /* Coordinator -> devices: ZCL Toggle to the router (direct) and to the
     * sleepy device (held until its poll). */
    {
        const uint8_t toggle[3] = {0x01, 0x42, 0x02};
        CHECK(zb_coord_send_aps(&coord, router.short_addr, 1, 1, 0x0006, 0x0104, toggle, 3, true) == ZB_SEND_OK);
        dev_rx_t rx;
        CHECK(dev_take(&router, &rx) && rx.a.cluster == 0x0006 && rx.a.ack_request && rx.len == 3 && rx.payload[2] == 0x02);
        unsigned sent = air_count;
        CHECK(zb_coord_send_aps(&coord, sleepy.short_addr, 1, 1, 0x0006, 0x0104, toggle, 3, false) == ZB_SEND_OK);
        CHECK(air_count == sent);                       /* indirect: nothing on air yet */
        dev_poll(&sleepy);
        CHECK(dev_take(&sleepy, &rx) && rx.a.cluster == 0x0006 && rx.payload[2] == 0x02);
        /* Unknown destination: held during the route discovery (as Z-Stack),
         * confirmed NWK_NO_ROUTE once ZB_DISC_WAIT_MS has passed (below). */
        CHECK(zb_coord_send_aps(&coord, 0x7777, 1, 1, 6, 0x0104, toggle, 3, false) == ZB_SEND_OK);
        CHECK(coord.stats.disc_buffered == 1 && coord.disc_count == 1);
    }

    /* Permit-join expiry closes the network and the beacon bit. */
    zb_coord_tick(&coord, 59999);
    CHECK(coord.permit && coord.mac.association_permit);
    zb_coord_tick(&coord, 60000);
    CHECK(!coord.permit && !coord.mac.association_permit);
    CHECK(coord.stats.disc_expired == 1 && coord.disc_count == 0);   /* no route ever found */
    /* Periodic link status / many-to-one request sent by the ticks: on air, done. */
    while (air_read < air_count) { air_read++; zb_coord_tx_done(&coord, MAC_TX_OK, false); }
    device_t late;
    dev_init(&late, 0x4444444444444444ull, TC_DEFAULT_LINK_KEY, 0x8E);
    join(&late, false);
    /* The first NWK-secured transmission reserved one durable counter window. */
    CHECK(reserved == 1000u + CORE_SECURITY_RESERVATION && coord.security.tx_next > 1000);
    /* ---- Reboot: everything comes back from the store ------------------- */
    {
        uint16_t devices_before = coord.devices.count;
        uint32_t nwk_used = coord.security.tx_next, aps_used = coord.tc_aps_frame_counter;
        uint16_t router_slot_before = nwk_devices_find_ext(&coord.devices, router.ext);
        uint8_t unique[16];
        memcpy(unique, coord.devices.dev[router_slot_before].link_key, 16);
        CHECK(coord.stats.store_failures == 0 && coord.stats.store_writes > 0);
        memset(&coord, 0xA5, sizeof coord);                  /* RAM lost */
        CHECK(nv_log_mount(&store, &STORE_OPS, 0, STORE_BANK, STORE_SECTOR));
        zb_coord_init(&coord, &RADIO, &UPPER, COORD_EXT, 0x99999999u, 1000);
        zb_coord_attach_store(&coord, &store);
        CHECK(zb_coord_restore(&coord));
        CHECK(coord.devices.count == devices_before && coord.stats.restored_devices == devices_before);
        CHECK(coord.mac.pan_id == PAN && coord.mac.channel == 15 && !memcmp(coord.network_key, nwk_key, 16));
        CHECK(coord.security.tx_next >= nwk_used && coord.tc_aps_frame_counter >= aps_used);
        uint16_t rs = nwk_devices_find_ext(&coord.devices, router.ext);
        CHECK(rs == router_slot_before && coord.devices.dev[rs].unique_key);
        CHECK(!memcmp(coord.devices.dev[rs].link_key, unique, 16) && coord.devices.dev[rs].authorized);
        CHECK(nwk_devices_find_short(&coord.devices, sleepy.short_addr) != NWK_DEVICE_NONE);
        CHECK(!coord.permit);                                   /* joining stays closed after reboot */

        /* The router keeps talking with its unique key, without re-pairing. */
        uint8_t hdr[8] = {0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, router.aps_counter++};
        const uint8_t report[4] = {0x18, 0x03, 0x0A, 0x00};
        nwk_aux_t aux = {.key_id = APS_KEY_DATA, .extended_nonce = true, .frame_counter = router.aps_fc++,
                         .source = router.ext};
        uint8_t aps[64];
        size_t an = aps_seal(router.tclk, hdr, sizeof hdr, &aux, report, sizeof report, aps, sizeof aps);
        unsigned before = aps_events;
        dev_send_nwk(&router, aps, an);
        CHECK(aps_events == before + 1);
        /* Coordinator frames after the reboot are NOT replays for the device:
         * its NWK counter resumed above every value used before. */
        const uint8_t toggle[3] = {0x01, 0x50, 0x02};
        air_read = air_count;
        CHECK(zb_coord_send_aps(&coord, router.short_addr, 1, 1, 0x0006, 0x0104, toggle, 3, false) == ZB_SEND_OK);
        dev_rx_t rx;
        CHECK(dev_take(&router, &rx) && rx.a.cluster == 0x0006 && rx.payload[1] == 0x50);
        /* Sleepy device: still reachable through its poll. */
        CHECK(zb_coord_send_aps(&coord, sleepy.short_addr, 1, 1, 0x0006, 0x0104, toggle, 3, false) == ZB_SEND_OK);
        dev_poll(&sleepy);
        CHECK(dev_take(&sleepy, &rx) && rx.payload[2] == 0x02);
        /* A brand-new reboot with an untouched store restores identically. */
        CHECK(nv_log_mount(&store, &STORE_OPS, 0, STORE_BANK, STORE_SECTOR));
    }
    /* A device holding the network key can forge the NWK security source:
     * announcing another IEEE with an address owned by someone else is then
     * indistinguishable from a real address conflict, which the coordinator
     * handles as Zigbee PRO requires: no join is reported, the conflict is
     * broadcast (Network Status 0x0D) and each child of ours using the
     * address is given a new one (unsolicited Rejoin Response). */
    {
        while (air_read < air_count) { air_read++; zb_coord_tx_done(&coord, MAC_TX_OK, false); }
        uint16_t router_slot = nwk_devices_find_ext(&coord.devices, router.ext);
        uint16_t before_short = coord.devices.dev[router_slot].short_addr;
        size_t fn = dev_announce(&router, sleepy.ext, mpdu);
        unsigned jb = joined_events;
        zb_coord_rx(&coord, mpdu, (uint8_t)fn, 180, -60);
        CHECK(joined_events == jb && coord.stats.address_conflicts == 1);
        CHECK(nwk_devices_find_ext(&coord.devices, sleepy.ext) != NWK_DEVICE_NONE);
        /* Both children use the conflicting address: both get a new one. */
        CHECK(coord.devices.dev[router_slot].short_addr != before_short && coord.stats.addresses_reassigned == 2);
        mac_frame_t f;
        CHECK(air_read < air_count && mac_frame_parse(air[air_read % 32], air_len[air_read % 32], &f) == MAC_OK);
        CHECK(f.dst_short == 0xFFFF);   /* Network Status broadcast first */
    }
    PASS("join_e2e");
    return 0;
}
