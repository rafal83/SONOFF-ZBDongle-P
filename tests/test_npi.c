/* SPDX-License-Identifier: MIT */
#include "diagnostic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned received;
static npi_frame last;
static void capture(void *context, const npi_frame *frame) {
    (void)context;
    received++;
    last = *frame;
}
static void feed(npi_parser *p, const uint8_t *bytes, size_t size, uint32_t now) {
    for (size_t i = 0; i < size; i++) npi_feed(p, bytes[i], now);
}
static void parser_tests(void) {
    npi_parser p;
    npi_init(&p, capture, NULL);
    const uint8_t ping[] = {0xfe, 0, 0x21, 1, 0x20};
    /* Golden vector, all split points, noise and adjacent frames. */
    for (size_t split = 0; split <= sizeof ping; split++) {
        unsigned before = received;
        feed(&p, ping, split, 0);
        feed(&p, ping + split, sizeof ping - split, 1);
        assert(received == before + 1 && last.cmd0 == 0x21 && last.length == 0);
    }
    npi_feed(&p, 0x99, 2);
    feed(&p, ping, sizeof ping, 3);
    feed(&p, ping, sizeof ping, 3);
    uint8_t bad[] = {0xfe, 0, 0x21, 1, 0x21};
    feed(&p, bad, sizeof bad, 4);
    assert(p.bad_fcs == 1);
    feed(&p, ping, sizeof ping, 5);
    npi_feed(&p, 0xfe, 6);
    npi_feed(&p, 251, 6);
    assert(p.oversized == 1);
    feed(&p, ping, sizeof ping, 7);
    npi_feed(&p, 0xfe, 8);
    npi_feed(&p, 5, 8);
    npi_expire(&p, 108);
    assert(p.timeouts == 1);
    feed(&p, ping, sizeof ping, 109);
    npi_feed(&p, 0xfe, UINT32_MAX - 10);
    npi_expire(&p, 100);
    assert(p.timeouts == 2); /* tick wraparound */
    npi_frame f = {.cmd0 = 0x61, .cmd1 = 0xfe, .length = 250};
    for (unsigned i = 0; i < 250; i++) f.payload[i] = (uint8_t)i;
    uint8_t bytes[NPI_MAX_FRAME];
    size_t size = npi_encode(bytes, sizeof bytes, &f);
    assert(size == 255 && npi_encode(bytes, 254, &f) == 0);
    feed(&p, bytes, size, 101);
    assert(last.length == 250 && memcmp(last.payload, f.payload, 250) == 0);
    f.length = 251;
    assert(npi_encode(bytes, sizeof bytes, &f) == 0);
    /* Deterministic noise stress: exercise every parser state, then idle-resync. */
    uint32_t seed = 7;
    for (unsigned i = 0; i < 200000; i++) {
        seed = seed * 1664525u + 1013904223u;
        npi_feed(&p, (uint8_t)(seed >> 24), 200 + i / 20);
    }
    npi_expire(&p, 20000);
    unsigned before = received;
    feed(&p, ping, sizeof ping, 20001);
    assert(received == before + 1);
}
static void queue_tests(void) {
    npi_tx_queue q;
    npi_tx_init(&q);
    for (unsigned cycle = 0; cycle < 30; cycle++) {
        for (uint8_t i = 0; i < NPI_TX_SLOTS; i++) {
            npi_frame f = {.cmd0 = 0x61, .cmd1 = i};
            assert(npi_tx_enqueue(&q, &f));
        }
        npi_frame extra = {0};
        assert(!npi_tx_enqueue(&q, &extra));
        npi_parser p;
        npi_init(&p, capture, NULL);
        uint8_t byte;
        unsigned count = 0;
        while (npi_tx_peek(&q, &byte)) {
            npi_feed(&p, byte, 0);
            npi_tx_consume(&q);
            if (++count % 5 == 0) assert(last.cmd1 == count / 5 - 1);
        }
        assert(count == 5 * NPI_TX_SLOTS && q.count == 0 && q.offset == 0);
        npi_tx_consume(&q);
    }
    assert(q.dropped == 30 && q.high_water == NPI_TX_SLOTS);
}
static npi_frame take_frame(npi_tx_queue *q) {
    npi_parser parser;
    npi_init(&parser, capture, NULL);
    unsigned before = received;
    uint8_t byte;
    while (received == before && npi_tx_peek(q, &byte)) {
        npi_feed(&parser, byte, 0);
        npi_tx_consume(q);
    }
    assert(received == before + 1);
    return last;
}
static void recovery_tests(void) {
    npi_parser p;
    const uint8_t ping[] = {0xfe, 0, 0x21, 1, 0x20};
    npi_init(&p, capture, NULL);
    unsigned before = received;
    for (unsigned i = 0; i < 300; i++) npi_feed(&p, 0xfe, 0);
    feed(&p, ping, sizeof ping, 0);
    assert(received == before + 1 && p.count == 0);

    /* A missing FCS consumes the next SOF: preserve it instead of losing ping. */
    npi_init(&p, capture, NULL);
    const uint8_t missing_fcs[] = {0xfe, 0, 0x21, 1, 0xfe};
    feed(&p, missing_fcs, sizeof missing_fcs, 0);
    feed(&p, ping + 1, sizeof ping - 1, 0);
    assert(p.accepted == 1 && p.bad_fcs == 1);

    /* A damaged complete frame must not discard an intact buffered suffix. */
    npi_init(&p, capture, NULL);
    const uint8_t damaged[] = {0xfe,8,0x21,0x77,0xfe,0,0x21,1,0x20,0x55,0x55,0x55,0};
    feed(&p, damaged, sizeof damaged, 0);
    assert(p.accepted == 1 && p.bad_fcs == 1 && last.cmd1 == 1);

    /* A bogus long length may conceal a ping until expiry; recover the suffix. */
    npi_init(&p, capture, NULL);
    const uint8_t long_prefix[] = {0xfe,250,0x21,3};
    feed(&p, long_prefix, sizeof long_prefix, 0);
    feed(&p, ping, sizeof ping, 1);
    assert(p.accepted == 0);
    npi_expire(&p, 101);
    assert(p.accepted == 1 && p.timeouts == 1 && p.count == 0);

    /* A valid payload may itself contain a full FE-framed ping. Never split it. */
    npi_init(&p, capture, NULL);
    npi_frame outer = {.cmd0 = 0x61, .cmd1 = 7, .length = 10};
    memcpy(outer.payload, ping, sizeof ping);
    uint8_t wire[NPI_MAX_FRAME];
    size_t size = npi_encode(wire, sizeof wire, &outer);
    feed(&p, wire, size, 0);
    assert(p.accepted == 1 && last.cmd1 == 7 && last.length == 10);

    /* Continuous slow input cannot keep a damaged frame alive indefinitely. */
    npi_init(&p, capture, NULL);
    feed(&p, long_prefix, sizeof long_prefix, 0);
    for (uint32_t now = 50; now <= NPI_FRAME_TIMEOUT_MS; now += 50) npi_feed(&p, 0, now);
    assert(p.timeouts == 1);
    feed(&p, ping, sizeof ping, NPI_FRAME_TIMEOUT_MS + 1);
    assert(p.accepted == 1);

    npi_init(&p, capture, NULL);
    feed(&p, long_prefix, sizeof long_prefix, 0);
    npi_rx_error(&p);
    feed(&p, ping, sizeof ping, 1);
    assert(p.rx_errors == 1 && p.accepted == 1 && p.resync_discarded == sizeof long_prefix);
}
static void priority_tests(void) {
    npi_tx_queue q;
    npi_tx_init(&q);
    npi_frame event = {.cmd0 = 0x45, .length = 0};
    npi_frame reply = {.cmd0 = 0x61, .cmd1 = 0x80, .length = 0};
    for (uint8_t i = 0; i < NPI_TX_SLOTS - NPI_TX_CONTROL_RESERVE; i++) {
        event.cmd1 = i;
        assert(npi_tx_enqueue(&q, &event));
    }
    assert(!npi_tx_enqueue(&q, &event));
    for (uint8_t i = 0; i < NPI_TX_CONTROL_RESERVE; i++) {
        reply.cmd1 = (uint8_t)(0x80 + i);
        assert(npi_tx_enqueue(&q, &reply));
    }
    assert(!npi_tx_enqueue(&q, &reply));
    assert(q.dropped_normal == 1 && q.dropped_control == 1);
    for (uint8_t i = 0; i < NPI_TX_CONTROL_RESERVE; i++) assert(take_frame(&q).cmd1 == 0x80 + i);
    for (uint8_t i = 0; i < NPI_TX_SLOTS - NPI_TX_CONTROL_RESERVE; i++) assert(take_frame(&q).cmd1 == i);

    /* Control priority must not starve a pending event. */
    npi_tx_init(&q);
    event.cmd1 = 0x55;
    assert(npi_tx_enqueue(&q, &event));
    for (uint8_t i = 0; i < NPI_TX_SLOTS - 1; i++) {
        reply.cmd1 = i;
        assert(npi_tx_enqueue(&q, &reply));
    }
    for (uint8_t i = 0; i < NPI_TX_CONTROL_BURST; i++) assert(take_frame(&q).cmd1 == i);
    assert(take_frame(&q).cmd1 == 0x55);
    for (uint8_t i = NPI_TX_CONTROL_BURST; i < NPI_TX_SLOTS - 1; i++) assert(take_frame(&q).cmd1 == i);

    /* Once peek selects a frame, a later reply cannot interleave its bytes. */
    npi_tx_init(&q);
    event.length = 250;
    for (unsigned i = 0; i < event.length; i++) event.payload[i] = (uint8_t)(255 - i);
    assert(npi_tx_enqueue(&q, &event));
    uint8_t byte;
    assert(npi_tx_peek(&q, &byte) && byte == 0xfe);
    assert(npi_tx_enqueue(&q, &reply));
    npi_frame first = take_frame(&q);
    assert(first.cmd0 == event.cmd0 && first.length == 250 && memcmp(first.payload, event.payload, 250) == 0);
    assert(take_frame(&q).cmd0 == 0x61);
    assert(q.count == 0 && q.normal_count == 0 && q.offset == 0);

    /* Reuse every pool slot repeatedly; no loss or aliasing after wraparound. */
    for (unsigned round = 0; round < 1000; round++) {
        for (uint8_t i = 0; i < NPI_TX_SLOTS; i++) {
            reply.cmd1 = i;
            assert(npi_tx_enqueue(&q, &reply));
        }
        for (uint8_t i = 0; i < NPI_TX_SLOTS; i++) assert(take_frame(&q).cmd1 == i);
    }
    npi_tx_init(&q);
    reply = (npi_frame){.cmd0 = 0x44, .cmd1 = 0x80}; /* AF dataConfirm */
    assert(npi_tx_enqueue(&q, &reply) && q.normal_count == 0);
    reply.cmd0 = 0x41; /* SYS resetInd */
    assert(npi_tx_enqueue(&q, &reply) && q.normal_count == 0);
    reply.cmd1 = 0x81;
    assert(npi_tx_enqueue(&q, &reply) && q.normal_count == 1);
}
static npi_frame request(diagnostic *d, uint8_t cmd0, uint8_t cmd1, uint8_t length) {
    npi_frame r = {.cmd0 = cmd0, .cmd1 = cmd1, .length = length};
    diagnostic_receive(d, &r);
    npi_parser p;
    npi_init(&p, capture, NULL);
    uint8_t byte;
    while (npi_tx_peek(&d->tx, &byte)) {
        npi_feed(&p, byte, 0);
        npi_tx_consume(&d->tx);
    }
    return last;
}
static void diagnostic_tests(void) {
    diagnostic d;
    const uint8_t ieee[] = {1, 2, 3, 4, 5, 6, 7, 8}; /* synthetic, never production */
    diagnostic_init(&d, ieee);
    npi_frame r = request(&d, 0x21, 1, 0);
    assert(r.cmd0 == 0x61 && r.length == 2 && r.payload[0] == 1 && r.payload[1] == 0);
    r = request(&d, 0x21, 2, 0);
    assert(r.length == 9 && r.payload[1] == 0xfe && r.payload[5] == 0);
    r = request(&d, 0x21, 4, 0);
    assert(r.length == 8 && memcmp(r.payload, ieee, 8) == 0);
    r = request(&d, 0x21, 1, 1);
    assert(r.cmd0 == 0x60 && r.cmd1 == 0 && r.payload[0] == 4);
    r = request(&d, 0x21, 0xff, 0);
    assert(r.payload[0] == 2);
    r = request(&d, 0x24, 1, 0);
    assert(r.payload[0] == 1 && r.payload[1] == 0x24);
    unsigned before = received;
    request(&d, 0x41, 0, 1);
    assert(received == before && d.ignored == 1);
    assert(d.unsupported == 3);
}
int main(void) {
    parser_tests();
    queue_tests();
    recovery_tests();
    priority_tests();
    diagnostic_tests();
    puts("PASS: MT vectors, fragments, resync storms/FCS/truncation, payload integrity, deadlines, UART errors, TX reserve/fairness/atomicity/pool reuse, diagnostics");
    return 0;
}
