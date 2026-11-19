/* SPDX-License-Identifier: MIT
 * Join path pieces: device table / address assignment, APS security and the
 * Trust Center Transport Key frame against a pyca/cryptography reference.
 */
#include "frt_test.h"
#include "aps_frame.h"
#include "aps_security.h"
#include "nwk_devices.h"
#include "trust_center.h"
#include "zb_mmo.h"
#include "vectors_crypto.h"

#define TC_EXT 0x00124B0001020304ull
#define DEV_EXT 0xA4C1380000000001ull

static nwk_devices_t table;

int main(void) {
    /* Key-transport key of the default TCLK: published value 4BAB0F17... */
    uint8_t ktk[16];
    zb_key_transport_key(TC_DEFAULT_LINK_KEY, ktk);
    const uint8_t published[16] = {0x4B, 0xAB, 0x0F, 0x17, 0x3E, 0x14, 0x34, 0xA2,
                                   0xD5, 0x72, 0xE1, 0xC1, 0xEF, 0x47, 0x87, 0x82};
    CHECK(!memcmp(ktk, published, 16) && !memcmp(ktk, TK_KEY_TRANSPORT_KEY, 16));

    /* Transport Key frame: byte-identical to the independent reference. */
    tc_transport_key_t p = {.tc_link_key = TC_DEFAULT_LINK_KEY, .network_key = NWK_KEY, .key_seq = 0,
                            .dst_short = 0x4E21, .dst_ext = DEV_EXT, .tc_ext = TC_EXT, .nwk_seq = 0x10,
                            .radius = 30, .aps_counter = TK_APS_COUNTER, .aps_frame_counter = TK_APS_FRAME_COUNTER};
    uint8_t frame[127];
    size_t n = tc_build_transport_network_key(&p, frame, sizeof frame);
    CHECK(n == sizeof TK_FRAME && !memcmp(frame, TK_FRAME, n));
    CHECK(tc_build_transport_network_key(&p, frame, n - 1) == 0);
    p.dst_short = 0xFFFC;
    CHECK(tc_build_transport_network_key(&p, frame, sizeof frame) == 0);

    /* Joining-device side: NWK header unsecured, APS opened with the KTK. */
    memcpy(frame, TK_FRAME, sizeof TK_FRAME);
    nwk_header_t nh;
    CHECK(nwk_parse(frame, sizeof TK_FRAME, &nh) == NWK_OK && !nh.security && nh.dst == 0x4E21);
    aps_header_t ah;
    CHECK(aps_parse(nh.payload, nh.payload_len, &ah) == APS_OK && ah.type == APS_TYPE_COMMAND && ah.security);
    uint8_t *aps = frame + nh.header_len;
    nwk_aux_t aux;
    size_t off = 0, plen = 0;
    CHECK(aps_open(ktk, aps, nh.payload_len, ah.header_len, &aux, &off, &plen));
    CHECK(aux.key_id == APS_KEY_TRANSPORT && aux.source == TC_EXT && aux.frame_counter == TK_APS_FRAME_COUNTER);
    CHECK(plen == 35 && aps[off] == APS_CMD_TRANSPORT_KEY && aps[off + 1] == APS_KEY_TYPE_STANDARD_NWK);
    CHECK(!memcmp(aps + off + 2, NWK_KEY, 16) && aps[off + 18] == 0);
    CHECK(aps[off + 19] == 0x01 && aps[off + 26] == 0xA4 && aps[off + 27] == 0x04 && aps[off + 34] == 0x00);
    /* Every single-byte alteration of the APS part is rejected. */
    for (size_t i = 0; i < nh.payload_len; i++) {
        memcpy(frame, TK_FRAME, sizeof TK_FRAME);
        frame[nh.header_len + i] ^= 0x04;
        CHECK(!aps_open(ktk, frame + nh.header_len, nh.payload_len, ah.header_len, &aux, &off, &plen));
    }
    /* Wrong key (the raw TCLK instead of the derived key). */
    memcpy(frame, TK_FRAME, sizeof TK_FRAME);
    CHECK(!aps_open(TC_DEFAULT_LINK_KEY, frame + nh.header_len, nh.payload_len, ah.header_len, &aux, &off, &plen));

    /* APS seal/open round trips for every key identifier. */
    for (uint8_t key_id = 0; key_id < 4; key_id++) {
        const uint8_t hdr[8] = {0x40, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0x22};
        const uint8_t msg[5] = {0x18, 0x01, 0x0A, 0x00, 0x00};
        nwk_aux_t a = {.key_id = key_id, .extended_nonce = true, .frame_counter = 77u + key_id,
                       .source = DEV_EXT, .key_seq = 3};
        uint8_t buf[64];
        size_t m = aps_seal(NWK_KEY, hdr, sizeof hdr, &a, msg, sizeof msg, buf, sizeof buf);
        CHECK(m == sizeof hdr + 13u + (key_id == 1 ? 1u : 0u) + sizeof msg + 4u && (buf[0] & 0x20));
        nwk_aux_t got;
        CHECK(aps_open(NWK_KEY, buf, m, sizeof hdr, &got, &off, &plen));
        CHECK(plen == sizeof msg && !memcmp(buf + off, msg, sizeof msg) && got.key_id == key_id);
        if (key_id == 1) CHECK(got.key_seq == 3);
    }
    nwk_aux_t short_nonce = {.key_id = 0, .extended_nonce = false};
    const uint8_t h2[2] = {0x01, 0x00};
    CHECK(aps_seal(NWK_KEY, h2, 2, &short_nonce, h2, 2, frame, sizeof frame) == 0);

    /* Device table and stochastic addressing. */
    nwk_devices_init(&table, 0xC0FFEEu);
    uint16_t a;
    CHECK(nwk_devices_associate(&table, DEV_EXT, NWK_CAP_ALLOC_ADDR, false, 0, &a) == NWK_ASSOC_ACCESS_DENIED);
    CHECK(a == 0xFFFF && table.count == 0);
    CHECK(nwk_devices_associate(&table, DEV_EXT, 0x8E, true, 5, &a) == NWK_ASSOC_SUCCESS);
    CHECK(a != 0 && a < 0xFFF8);
    uint16_t again;
    CHECK(nwk_devices_associate(&table, DEV_EXT, 0x8E, true, 6, &again) == NWK_ASSOC_SUCCESS && again == a);
    CHECK(table.count == 1 && nwk_devices_find_short(&table, a) == nwk_devices_find_ext(&table, DEV_EXT));
    CHECK(!table.dev[nwk_devices_find_ext(&table, DEV_EXT)].authorized);
    static uint16_t addrs[NWK_MAX_DEVICES];
    addrs[0] = a;
    for (unsigned i = 1; i < NWK_MAX_DEVICES; i++) {
        CHECK(nwk_devices_associate(&table, DEV_EXT + i, 0x80, true, 0, &addrs[i]) == NWK_ASSOC_SUCCESS);
        CHECK(addrs[i] != 0 && addrs[i] < 0xFFF8);
        for (unsigned k = 0; k < i; k++) CHECK(addrs[k] != addrs[i]);
    }
    CHECK(nwk_devices_associate(&table, 0x1111, 0x80, true, 0, &a) == NWK_ASSOC_PAN_AT_CAPACITY);
    CHECK(nwk_devices_remove(&table, DEV_EXT + 5) && !nwk_devices_remove(&table, DEV_EXT + 5));
    CHECK(nwk_devices_find_short(&table, addrs[5]) == CORE_INDEX_NONE);
    CHECK(nwk_devices_associate(&table, 0x1111, 0x80, true, 0, &a) == NWK_ASSOC_SUCCESS);

    /* Learn: an address change follows the device; conflicts are refused. */
    CHECK(nwk_devices_remove(&table, DEV_EXT + 6));
    uint16_t free_addr = 0x0042;
    while (nwk_devices_find_short(&table, free_addr) != CORE_INDEX_NONE) free_addr++;
    CHECK(nwk_devices_learn(&table, DEV_EXT + 7, free_addr, NWK_REL_NEIGHBOR));
    uint16_t s7 = nwk_devices_find_ext(&table, DEV_EXT + 7);
    CHECK(table.dev[s7].short_addr == free_addr && nwk_devices_find_short(&table, free_addr) == s7);
    CHECK(nwk_devices_find_short(&table, addrs[7]) == CORE_INDEX_NONE);
    CHECK(table.dev[s7].authorized && table.dev[s7].relationship == NWK_REL_NEIGHBOR);
    CHECK(!nwk_devices_learn(&table, DEV_EXT + 8, free_addr, NWK_REL_NONE));  /* conflict */
    CHECK(!nwk_devices_learn(&table, DEV_EXT + 8, 0x0000, NWK_REL_NONE));
    CHECK(!nwk_devices_learn(&table, DEV_EXT + 8, 0xFFFD, NWK_REL_NONE));

    /* Randomised mutations: both indexes stay consistent with the table. */
    for (unsigned round = 0; round < 50000; round++) {
        uint64_t ext = DEV_EXT + frt_rand() % 260;
        if (frt_rand() & 1) (void)nwk_devices_associate(&table, ext, 0x80, true, round, &a);
        else (void)nwk_devices_remove(&table, ext);
        if (round % 997 == 0) {
            unsigned seen = 0;
            for (unsigned i = 0; i < NWK_MAX_DEVICES; i++) {
                if (table.dev[i].ext && nwk_devices_find_ext(&table, table.dev[i].ext) == i) {
                    seen++;
                    CHECK(nwk_devices_find_short(&table, table.dev[i].short_addr) == i);
                }
            }
            CHECK(seen == table.count);
        }
    }
    PASS("nwk_join");
    return 0;
}
