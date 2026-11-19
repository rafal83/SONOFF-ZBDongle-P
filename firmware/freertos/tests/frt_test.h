/* SPDX-License-Identifier: MIT - minimal host test harness. */
#ifndef FRT_TEST_H
#define FRT_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned frt_checks;
#define CHECK(c) do { frt_checks++; if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define PASS(name) printf("%s: %u checks passed\n", name, frt_checks)
/* xorshift32: deterministic fuzz input. */
static unsigned frt_rng_state = 0x12345678u;
static unsigned frt_rand(void) {
    unsigned x = frt_rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return frt_rng_state = x;
}
#endif
