/* SPDX-License-Identifier: MIT
 * UART noise bench of the host link (firmware/src/npi.c), the workload SMLIGHT
 * publishes for ZeroStack ("hostile UART noise": SYS_PING probe, SOF storms,
 * random noise, truncated frames; stock Z-Stack 8.84 s blackout and 44 lost
 * pings, ZeroStack 0 lost, 3-55 ms recovery).
 *
 * Byte timing of the production link: 1 Mbaud, 10 us per byte; the host task
 * calls npi_expire() every millisecond. A SYS_PING (21 01) is sent every
 * 20 ms; each scenario injects its noise between two pings. Measured: pings
 * lost, and the recovery time = first ping parsed after the end of the noise.
 * Found: a noise burst rich in SOF bytes blocked the link for seconds (each
 * fake header got a fresh frame timeout); fixed in npi.c (frame age kept on
 * noise, rescan when a candidate stalls behind a complete valid frame).
 */
#include "frt_test.h"
#include "npi.h"

#define BYTE_US 10u
#define PING_EVERY_US 20000u

static npi_parser parser;
static uint64_t now_us;
static unsigned pings_parsed;
static uint64_t last_ping_us;
static uint32_t lcg = 12345u;
static uint8_t rnd(void) { lcg = lcg * 1103515245u + 12345u; return (uint8_t)(lcg >> 16); }

static void on_frame(void *ctx, const npi_frame *f) {
    (void)ctx;
    if (f->cmd0 == 0x21 && f->cmd1 == 0x01 && f->length == 0) { pings_parsed++; last_ping_us = now_us; }
}

static void advance(uint64_t us) {
    uint64_t end = now_us + us;
    while (now_us < end) {
        uint64_t step = end - now_us < 1000u ? end - now_us : 1000u;
        now_us += step;
        npi_expire(&parser, (uint32_t)(now_us / 1000u));
    }
}
static void send(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        npi_feed(&parser, b[i], (uint32_t)(now_us / 1000u));
        advance(BYTE_US);
    }
}
static void ping(void) {
    static const uint8_t p[5] = {0xFE, 0x00, 0x21, 0x01, 0x20};   /* FCS = 00 ^ 21 ^ 01 */
    send(p, sizeof p);
}

typedef struct { const char *name; unsigned lost; double recovery_ms; } result_t;

/* 10 pings before, the noise, then 40 pings every 20 ms (800 ms); the first
 * one after the noise starts right after it. */
static result_t run(const char *name, const uint8_t *noise, size_t n) {
    npi_init(&parser, on_frame, 0);
    now_us = 0;
    pings_parsed = 0;
    unsigned sent = 0;
    for (unsigned i = 0; i < 10; i++) { ping(); sent++; advance(PING_EVERY_US); }
    send(noise, n);
    uint64_t noise_end = now_us;
    unsigned before = pings_parsed;
    double recovery = -1;
    for (unsigned i = 0; i < 40; i++) {
        unsigned k = pings_parsed;
        ping();
        sent++;
        if (recovery < 0 && pings_parsed > k) recovery = (double)(last_ping_us - noise_end) / 1000.0;
        advance(PING_EVERY_US);
        if (recovery < 0 && pings_parsed > k) recovery = (double)(last_ping_us - noise_end) / 1000.0;
    }
    (void)before;
    result_t r = {name, sent - pings_parsed, recovery};
    printf("uart_noise: %-34s %u ping(s) lost, recovery %.2f ms\n", name, r.lost, r.recovery_ms);
    return r;
}

int main(void) {
    static uint8_t noise[4096];
    result_t r[6];

    /* 1. 300 x SOF */
    for (unsigned i = 0; i < 300; i++) noise[i] = 0xFE;
    r[0] = run("300 x SOF storm", noise, 300);

    /* 2. 2 KiB of random bytes */
    for (unsigned i = 0; i < 2048; i++) noise[i] = rnd();
    r[1] = run("2 KiB random noise", noise, 2048);

    /* 3. truncated frames: SOF, a length, a few bytes, then nothing */
    size_t n = 0;
    for (unsigned f = 0; f < 20; f++) {
        noise[n++] = 0xFE; noise[n++] = (uint8_t)(10 + f); noise[n++] = 0x21; noise[n++] = 0x02;
        for (unsigned k = 0; k < f % 7; k++) noise[n++] = rnd();
    }
    r[2] = run("20 truncated frames", noise, n);

    /* 4. complete frames with a wrong FCS */
    n = 0;
    for (unsigned f = 0; f < 20; f++) {
        noise[n++] = 0xFE; noise[n++] = 2; noise[n++] = 0x21; noise[n++] = 0x09;
        noise[n++] = rnd(); noise[n++] = rnd(); noise[n++] = 0x55;
    }
    r[3] = run("20 frames with a bad FCS", noise, n);

    /* 5. a header announcing the largest frame, then silence of the host */
    noise[0] = 0xFE; noise[1] = 250; noise[2] = 0x21; noise[3] = 0x02;
    r[4] = run("false 250-byte frame header", noise, 4);

    /* 6. noise bursts glued to the pings (no gap before the next ping) */
    n = 0;
    for (unsigned k = 0; k < 64; k++) noise[n++] = (uint8_t)(k % 3 ? rnd() : 0xFE);
    r[5] = run("64 noise bytes glued to a ping", noise, n);

    unsigned lost = 0;
    double worst = 0;
    for (unsigned i = 0; i < 6; i++) {
        lost += r[i].lost;
        if (r[i].recovery_ms > worst) worst = r[i].recovery_ms;
        CHECK(r[i].recovery_ms >= 0);   /* the link always comes back */
    }
    printf("uart_noise: 6 scenarios, %u ping(s) lost in total, worst recovery %.2f ms\n", lost, worst);
    CHECK(lost == 0 && worst < 60.0);   /* SMLIGHT publishes 0 lost, 3-55 ms for ZeroStack */
    PASS("uart_noise");
    return 0;
}
