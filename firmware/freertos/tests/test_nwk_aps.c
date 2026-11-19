/* SPDX-License-Identifier: MIT */
#include "frt_test.h"
#include "nwk_frame.h"
#include "aps_frame.h"
#include "vectors_crypto.h"

static void nwk_roundtrip(const nwk_header_t *h) {
    uint8_t buf[128];
    size_t n = nwk_build(h, buf, sizeof buf);
    CHECK(n > 0);
    buf[n] = 0x99; /* one payload byte */
    nwk_header_t p;
    CHECK(nwk_parse(buf, n + 1, &p) == NWK_OK);
    CHECK(p.header_len == n && p.payload_len == 1 && p.payload[0] == 0x99);
    CHECK(p.type == h->type && p.dst == h->dst && p.src == h->src && p.radius == h->radius);
    CHECK(p.seq == h->seq && p.security == h->security && p.discover_route == h->discover_route);
    CHECK(p.has_dst_ieee == h->has_dst_ieee && p.has_src_ieee == h->has_src_ieee);
    if (h->has_dst_ieee) CHECK(p.dst_ieee == h->dst_ieee);
    if (h->has_src_ieee) CHECK(p.src_ieee == h->src_ieee);
    CHECK(p.multicast == h->multicast && p.multicast_control == (h->multicast ? h->multicast_control : 0));
    CHECK(p.source_route == h->source_route);
    if (h->source_route) {
        CHECK(p.relay_count == h->relay_count && p.relay_index == h->relay_index);
        for (unsigned i = 0; i < h->relay_count; i++) CHECK(p.relays[i] == h->relays[i]);
    }
    for (size_t cut = 0; cut < n; cut++) CHECK(nwk_parse(buf, cut, &p) < 0);
}

static void aps_roundtrip(const aps_header_t *h, size_t expected_len) {
    uint8_t buf[64];
    size_t n = aps_build(h, buf, sizeof buf);
    CHECK(n == expected_len);
    buf[n] = 0x42;
    aps_header_t p;
    CHECK(aps_parse(buf, n + 1, &p) == APS_OK);
    CHECK(p.header_len == n && p.payload_len == 1 && p.payload[0] == 0x42);
    CHECK(p.type == h->type && p.delivery == h->delivery && p.ack_request == h->ack_request);
    CHECK(p.counter == h->counter && p.security == h->security && p.ack_format == h->ack_format);
    for (size_t cut = 0; cut < n; cut++) CHECK(aps_parse(buf, cut, &p) < 0);
}

int main(void) {
    nwk_header_t h = {0};
    h.type = NWK_TYPE_DATA;
    h.dst = 0x0000; h.src = 0x4E21; h.radius = 30; h.seq = 0x7A; h.discover_route = 1;
    nwk_roundtrip(&h);
    h.security = true; h.has_src_ieee = true; h.src_ieee = 0x00124B0001020304ull;
    nwk_roundtrip(&h);
    h.has_dst_ieee = true; h.dst_ieee = 0xA4C1380011223344ull;
    nwk_roundtrip(&h);
    h.multicast = true; h.multicast_control = 0x15;
    nwk_roundtrip(&h);
    h.source_route = true; h.relay_count = 3; h.relay_index = 2;
    h.relays[0] = 0x1111; h.relays[1] = 0x2222; h.relays[2] = 0x3333;
    nwk_roundtrip(&h);
    h.type = NWK_TYPE_COMMAND;
    nwk_roundtrip(&h);
    h.relay_index = 3;
    uint8_t buf[160];
    CHECK(nwk_build(&h, buf, sizeof buf) == 0);
    h.relay_count = NWK_MAX_RELAYS + 1;
    CHECK(nwk_build(&h, buf, sizeof buf) == 0);

    /* Header of the generated secured frame. */
    nwk_header_t p;
    CHECK(nwk_parse(NWK_SECURED_FRAME, sizeof NWK_SECURED_FRAME, &p) == NWK_OK);
    CHECK(p.security && p.has_src_ieee && p.header_len == NWK_SECURED_HEADER_LEN && p.src == 0x1234);
    nwk_aux_t aux;
    CHECK(nwk_aux_parse(p.payload, p.payload_len, &aux) == NWK_OK);
    CHECK(aux.level == 0 && aux.key_id == 1 && aux.extended_nonce && aux.len == 14);
    CHECK(aux.frame_counter == NWK_SECURED_COUNTER);
    uint8_t aux_buf[16];
    CHECK(nwk_aux_build(&aux, aux_buf, sizeof aux_buf) == 14 && !memcmp(aux_buf, p.payload, 14));
    uint8_t nonce[13];
    nwk_nonce(aux.source, aux.frame_counter, 0x2D, nonce);
    CHECK(!memcmp(nonce, NWK_SRC_IEEE, 8) && nonce[8] == 0x45 && nonce[12] == 0x2D);

    /* Reserved/unsupported encodings. */
    uint8_t raw[8] = {0x0A, 0, 0, 0, 0, 0, 1, 1};          /* frame type 2 */
    CHECK(nwk_parse(raw, 8, &p) == NWK_ERR_RESERVED);
    raw[0] = 0x04;                                          /* protocol version 1 */
    CHECK(nwk_parse(raw, 8, &p) == NWK_ERR_UNSUPPORTED);
    raw[0] = 0x08; raw[1] = 0x40;                           /* reserved bit 14 */
    CHECK(nwk_parse(raw, 8, &p) == NWK_ERR_RESERVED);
    uint8_t sr[12] = {0x08, 0x04, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}; /* zero relays */
    CHECK(nwk_parse(sr, 12, &p) == NWK_ERR_RESERVED);
    CHECK(nwk_is_broadcast(0xFFFD) && nwk_is_broadcast(0xFFFF) && !nwk_is_broadcast(0x0000));

    /* APS: unicast data with ACK request (ZCL). */
    aps_header_t a = {0};
    a.type = APS_TYPE_DATA; a.delivery = APS_DELIVERY_UNICAST; a.ack_request = true;
    a.dst_endpoint = 1; a.src_endpoint = 1; a.cluster = 0x0006; a.profile = 0x0104; a.counter = 0x22;
    aps_roundtrip(&a, 8);
    a.delivery = APS_DELIVERY_GROUP; a.ack_request = false; a.group = 0x1234;
    aps_roundtrip(&a, 9);
    a.delivery = APS_DELIVERY_BROADCAST;
    aps_roundtrip(&a, 8);
    a.delivery = APS_DELIVERY_UNICAST; a.extended = true; a.fragmentation = APS_FRAG_FIRST; a.block_number = 3;
    aps_roundtrip(&a, 10);
    aps_header_t c = {0};
    c.type = APS_TYPE_COMMAND; c.security = true; c.counter = 9;
    aps_roundtrip(&c, 2);
    aps_header_t ack = {0};
    ack.type = APS_TYPE_ACK; ack.dst_endpoint = 1; ack.src_endpoint = 1; ack.cluster = 6;
    ack.profile = 0x0104; ack.counter = 0x22;
    aps_roundtrip(&ack, 8);
    ack.ack_format = true;
    aps_roundtrip(&ack, 2);
    ack.ack_format = false; ack.extended = true; ack.fragmentation = APS_FRAG_NEXT; ack.block_number = 1;
    ack.ack_bitfield = 0x0F;
    aps_roundtrip(&ack, 11);
    uint8_t aps_raw[4] = {0x04, 0, 0, 0};                   /* indirect delivery */
    aps_header_t ap;
    CHECK(aps_parse(aps_raw, 4, &ap) == APS_ERR_RESERVED);
    aps_raw[0] = 0x0D;                                      /* group command */
    CHECK(aps_parse(aps_raw, 4, &ap) == APS_ERR_RESERVED);
    aps_raw[0] = 0x03;                                      /* inter-PAN */
    CHECK(aps_parse(aps_raw, 4, &ap) == APS_ERR_UNSUPPORTED);

    for (unsigned i = 0; i < 200000; i++) {
        size_t len = frt_rand() % 128;
        for (size_t k = 0; k < len; k++) buf[k] = (uint8_t)frt_rand();
        if (i & 1) buf[0] = (uint8_t)((buf[0] & 0xC3u) | 0x08u);  /* bias towards valid NWK */
        if (nwk_parse(buf, len, &p) == NWK_OK) {
            CHECK(p.header_len + p.payload_len == len);
            (void)nwk_aux_parse(p.payload, p.payload_len, &aux);
        }
        if (aps_parse(buf, len, &ap) == APS_OK) CHECK(ap.header_len + ap.payload_len == len);
    }
    PASS("nwk_aps");
    return 0;
}
