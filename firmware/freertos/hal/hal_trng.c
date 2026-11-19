/* SPDX-License-Identifier: MIT */
#include "hal_trng.h"
#include "hal_sys.h"
#include "zb_mmo.h"

#include <inc/hw_memmap.h>
#include <inc/hw_types.h>
#include <driverlib/trng.h>

#define TRNG_SAMPLES_PER_CYCLE 240000u   /* TI TRNGCC26XX default */
#define TRNG_WAIT_CYCLES (48000000u / 10u) /* 100 ms per 64-bit word */

static bool started;

static bool next_word(uint32_t *lo, uint32_t *hi) {
    if (!started) {
        TRNGReset();
        TRNGConfigure(0, TRNG_SAMPLES_PER_CYCLE, 0);
        TRNGEnable();
        started = true;
    }
    uint32_t start = hal_cycles();
    while (!(TRNGStatusGet() & TRNG_NUMBER_READY)) {
        if (hal_cycles() - start > TRNG_WAIT_CYCLES) return false;
    }
    *lo = TRNGNumberGet(TRNG_LOW_WORD);
    *hi = TRNGNumberGet(TRNG_HI_WORD);   /* reading HI acknowledges the number */
    return true;
}

bool hal_trng_read(uint8_t *out, size_t len) {
    size_t n = 0;
    while (n < len) {
        uint32_t w[2];
        if (!next_word(&w[0], &w[1])) return false;
        for (unsigned i = 0; i < 8 && n < len; i++) out[n++] = (uint8_t)(w[i / 4] >> (8u * (i % 4)));
    }
    return true;
}

bool hal_trng_key(uint8_t key[16]) {
    uint8_t raw[32];
    bool ok = hal_trng_read(raw, sizeof raw) && zb_mmo_hash(raw, sizeof raw, key);
    volatile uint8_t *wipe = raw;
    for (unsigned i = 0; i < sizeof raw; i++) wipe[i] = 0;
    return ok;
}

uint32_t hal_trng_u32(void) {
    uint8_t b[4] = {0};
    (void)hal_trng_read(b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
