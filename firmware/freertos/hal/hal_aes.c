/* SPDX-License-Identifier: MIT
 * CC2652P AES-128 accelerator, single-block ECB encryption for CCM*.
 *
 * Register-level, after the sequence of TI's polled AESECBCC26XX driver
 * (key store area 6, DMA in/out, RESULT_AVAIL), but EVERY wait is bounded:
 * TI's AESWriteToKeyStore / AESReadFromKeyStore / AESStartDMAOperation /
 * AESDMAReset spin forever if the engine stops answering, which would block
 * the radio task. On any error (timeout, DMA bus error, key store error) the
 * engine and its DMA are reset, the key store area is invalidated and the
 * key cache forgotten; the caller then redoes the operation in software.
 *
 * Concurrency: one operation at a time, enforced by a try-lock (a second
 * caller gets `false` immediately and falls back to software, it never
 * waits). The key cache is only valid while the crypto domain stays powered:
 * the firmware never enters standby (configUSE_TICKLESS_IDLE 0); a future
 * standby must call hal_aes_invalidate() on wake-up.
 *
 * Built with HAL_AES_HOST_MODEL, the registers go through crypto_rd/crypto_wr
 * so that tests/test_hal_aes.c runs this exact code against a model of the
 * peripheral with injected faults.
 *
 * Status: tested on host against the model; NOT yet executed on the MCU. Only
 * linked into the `--hw-aes` firmware variant, and enabled there only after
 * hal_aes_selftest() passed at boot.
 */
#include "hal_aes.h"
#include "aes128.h"

#ifdef HAL_AES_HOST_MODEL
#include "inc/hw_crypto.h"
uint32_t crypto_rd(uint32_t off);
void crypto_wr(uint32_t off, uint32_t value);
uint32_t crypto_model_cycles(void);
uint32_t crypto_model_addr(const void *p);   /* 32-bit handle for a host pointer */
#define RD(off) crypto_rd(off)
#define WR(off, v) crypto_wr((off), (v))
#define CYCLES() crypto_model_cycles()
#define DMA_ADDR(p) crypto_model_addr(p)
#else
#include "hal_sys.h"
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_crypto.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_types.h)
#define RD(off) HWREG(CRYPTO_BASE + (off))
#define WR(off, v) (HWREG(CRYPTO_BASE + (off)) = (v))
#define CYCLES() hal_cycles()
#define DMA_ADDR(p) ((uint32_t)(p))
#endif

#define KEY_AREA 6u
#define WAIT_BUDGET_CYCLES 48000u   /* 1 ms at 48 MHz; a block takes a few µs */
#define ERR_FLAGS (CRYPTO_IRQSTAT_DMA_BUS_ERR_M | CRYPTO_IRQSTAT_KEY_ST_WR_ERR_M | CRYPTO_IRQSTAT_KEY_ST_RD_ERR_M)

static uint8_t cached_key[16];
static bool key_loaded;
/* Word aligned DMA buffers in SRAM; the key buffer is wiped after use. */
static uint32_t dma_in[4], dma_out[4], dma_key[4];
static hal_aes_stats_t stats;
static volatile uint32_t busy;

static bool try_lock(void) { return __atomic_exchange_n(&busy, 1u, __ATOMIC_ACQUIRE) == 0u; }
static void unlock(void) { __atomic_store_n(&busy, 0u, __ATOMIC_RELEASE); }

static void wipe(void *p, unsigned n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

/* Wait until (reg & mask) == want, bounded. */
static bool wait_reg(uint32_t off, uint32_t mask, uint32_t want) {
    uint32_t start = CYCLES();
    while ((RD(off) & mask) != want)
        if (CYCLES() - start > WAIT_BUDGET_CYCLES) return false;
    return true;
}

/* Wait for any of `flags` or an error flag; clears what it saw. */
static bool wait_flags(uint32_t flags, uint32_t *got) {
    uint32_t start = CYCLES();
    for (;;) {
        uint32_t s = RD(CRYPTO_O_IRQSTAT) & (flags | ERR_FLAGS);
        if (s) {
            WR(CRYPTO_O_IRQCLR, s);
            *got = s;
            return true;
        }
        if (CYCLES() - start > WAIT_BUDGET_CYCLES) return false;
    }
}

/* Engine back to a known idle state after any error. Bounded. */
static void recover(void) {
    stats.resets++;
    WR(CRYPTO_O_DMASWRESET, CRYPTO_DMASWRESET_SWRES);
    if (!wait_reg(CRYPTO_O_DMASTAT, CRYPTO_DMASTAT_CH0_ACT | CRYPTO_DMASTAT_CH1_ACT, 0)) stats.reset_timeouts++;
    WR(CRYPTO_O_SWRESET, CRYPTO_SWRESET_SW_RESET);
    WR(CRYPTO_O_AESCTL, 0);
    WR(CRYPTO_O_AESDATALEN0, 0);
    WR(CRYPTO_O_AESDATALEN1, 0);
    WR(CRYPTO_O_AESAUTHLEN, 0);
    WR(CRYPTO_O_KEYWRITTENAREA, 1u << KEY_AREA);   /* invalidate our key area */
    WR(CRYPTO_O_IRQCLR, 0xFFFFFFFFu);
    WR(CRYPTO_O_ALGSEL, 0);
    key_loaded = false;
    wipe(cached_key, sizeof cached_key);
    wipe(dma_key, sizeof dma_key);
}

/* AESStartDMAOperation, bounded. */
static bool start_dma(const void *src, uint32_t len, void *dst, uint32_t dlen) {
    WR(CRYPTO_O_IRQCLR, CRYPTO_IRQCLR_RESULT_AVAIL | CRYPTO_IRQCLR_DMA_IN_DONE);
    if (!wait_reg(CRYPTO_O_IRQSTAT, CRYPTO_IRQSTAT_DMA_IN_DONE_M | CRYPTO_IRQSTAT_RESULT_AVAIL_M, 0)) return false;
    WR(CRYPTO_O_DMACH0CTL, RD(CRYPTO_O_DMACH0CTL) | (1u << CRYPTO_DMACH0CTL_EN_BITN));
    WR(CRYPTO_O_DMACH0EXTADDR, DMA_ADDR(src));
    WR(CRYPTO_O_DMACH0LEN, len);
    if (dst && dlen) {
        WR(CRYPTO_O_DMACH1CTL, RD(CRYPTO_O_DMACH1CTL) | (1u << CRYPTO_DMACH1CTL_EN_BITN));
        WR(CRYPTO_O_DMACH1EXTADDR, DMA_ADDR(dst));
        WR(CRYPTO_O_DMACH1LEN, dlen);
    }
    return true;
}

/* AESWriteToKeyStore (128-bit, area 6), bounded. */
static bool key_write(const uint8_t key[16]) {
    uint8_t *k = (uint8_t *)dma_key;
    for (unsigned i = 0; i < 16; i++) k[i] = key[i];
    WR(CRYPTO_O_KEYWRITTENAREA, 1u << KEY_AREA);   /* invalidate first */
    WR(CRYPTO_O_IRQTYPE, CRYPTO_IRQTYPE_LEVEL_M);
    WR(CRYPTO_O_IRQEN, CRYPTO_IRQEN_DMA_IN_DONE_M | CRYPTO_IRQEN_RESULT_AVAIL_M);
    WR(CRYPTO_O_ALGSEL, CRYPTO_ALGSEL_KEY_STORE);
    WR(CRYPTO_O_IRQCLR, CRYPTO_IRQCLR_DMA_IN_DONE | CRYPTO_IRQCLR_RESULT_AVAIL);
    if (RD(CRYPTO_O_KEYSIZE) != CRYPTO_KEYSIZE_SIZE_128_BIT) WR(CRYPTO_O_KEYSIZE, CRYPTO_KEYSIZE_SIZE_128_BIT);
    WR(CRYPTO_O_KEYWRITEAREA, 1u << KEY_AREA);
    uint32_t got = 0;
    bool ok = start_dma(dma_key, 16, 0, 0);
    if (!ok) stats.timeouts++;
    else if (!wait_flags(CRYPTO_IRQSTAT_RESULT_AVAIL_M | CRYPTO_IRQSTAT_DMA_IN_DONE_M, &got)) { stats.timeouts++; ok = false; }
    else if ((got & ERR_FLAGS) || !(RD(CRYPTO_O_KEYWRITTENAREA) & (1u << KEY_AREA))) { stats.key_errors++; ok = false; }
    wipe(dma_key, sizeof dma_key);
    WR(CRYPTO_O_ALGSEL, 0);
    return ok;
}

/* AESReadFromKeyStore (area 6 into the AES engine), bounded. */
static bool key_select(void) {
    if (!(RD(CRYPTO_O_KEYWRITTENAREA) & (1u << KEY_AREA))) { stats.key_errors++; return false; }
    WR(CRYPTO_O_KEYREADAREA, KEY_AREA);
    if (!wait_reg(CRYPTO_O_KEYREADAREA, CRYPTO_KEYREADAREA_BUSY_M, 0)) { stats.timeouts++; return false; }
    if (RD(CRYPTO_O_IRQSTAT) & CRYPTO_IRQSTAT_KEY_ST_RD_ERR_M) { stats.key_errors++; return false; }
    return true;
}

static bool block_locked(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    bool same = key_loaded;
    for (unsigned i = 0; i < 16 && same; i++) same = cached_key[i] == key[i];
    if (!same) {
        key_loaded = false;
        if (!key_write(key)) return false;
        for (unsigned i = 0; i < 16; i++) cached_key[i] = key[i];
        key_loaded = true;
        stats.key_loads++;
    }
    WR(CRYPTO_O_IRQCLR, CRYPTO_IRQCLR_RESULT_AVAIL | CRYPTO_IRQCLR_DMA_IN_DONE | CRYPTO_IRQSTAT_DMA_BUS_ERR_M);
    WR(CRYPTO_O_IRQEN, RD(CRYPTO_O_IRQEN) & ~CRYPTO_IRQEN_DMA_IN_DONE_M);
    WR(CRYPTO_O_ALGSEL, CRYPTO_ALGSEL_AES_M);
    if (!key_select()) return false;
    WR(CRYPTO_O_AESCTL, CRYPTO_AESCTL_DIR);   /* ECB encrypt */
    WR(CRYPTO_O_AESDATALEN0, 16);
    WR(CRYPTO_O_AESDATALEN1, 0);
    WR(CRYPTO_O_AESAUTHLEN, 0);
    uint8_t *bin = (uint8_t *)dma_in;
    for (unsigned i = 0; i < 16; i++) bin[i] = in[i];
    if (!start_dma(dma_in, 16, dma_out, 16)) { stats.timeouts++; return false; }
    uint32_t got = 0;
    if (!wait_flags(CRYPTO_IRQSTAT_RESULT_AVAIL_M, &got)) { stats.timeouts++; return false; }
    if (got & ERR_FLAGS) { stats.dma_errors++; return false; }
    /* The output DMA channel must have finished writing dma_out. */
    if (!wait_reg(CRYPTO_O_DMASTAT, CRYPTO_DMASTAT_CH1_ACT, 0)) { stats.timeouts++; return false; }
    WR(CRYPTO_O_ALGSEL, 0);
    const uint8_t *bout = (const uint8_t *)dma_out;
    for (unsigned i = 0; i < 16; i++) out[i] = bout[i];
    wipe(dma_in, sizeof dma_in);
    wipe(dma_out, sizeof dma_out);
    stats.blocks++;
    return true;
}

bool hal_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    if (!try_lock()) { stats.busy++; return false; }
    bool ok = block_locked(key, in, out);
    if (!ok) {
        recover();
        wipe(dma_in, sizeof dma_in);
        wipe(dma_out, sizeof dma_out);
    }
    unlock();
    return ok;
}

void hal_aes_invalidate(void) {
    if (!try_lock()) return;   /* the running operation reloads the key */
    key_loaded = false;
    wipe(cached_key, sizeof cached_key);
    WR(CRYPTO_O_KEYWRITTENAREA, 1u << KEY_AREA);
    unlock();
}

bool hal_aes_selftest(void) {
    /* FIPS-197 C.1, then random blocks and keys against the software AES. */
    static const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    static const uint8_t ct[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
                                   0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
    uint8_t out[16];
    if (!hal_aes_block(key, pt, out)) return false;
    for (unsigned i = 0; i < 16; i++) if (out[i] != ct[i]) return false;
    uint8_t k2[16], blk[16], sw[16];
    uint32_t x = CYCLES() | 1u;
    for (unsigned round = 0; round < 8; round++) {
        for (unsigned i = 0; i < 16; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; k2[i] = (uint8_t)x; blk[i] = (uint8_t)(x >> 8); }
        aes128_ctx_t a;
        aes128_init(&a, k2);
        aes128_encrypt(&a, blk, sw);
        aes128_clear(&a);
        /* Same key twice: the second block exercises the cached key path. */
        for (unsigned rep = 0; rep < 2; rep++) {
            if (!hal_aes_block(k2, blk, out)) return false;
            for (unsigned i = 0; i < 16; i++) if (out[i] != sw[i]) return false;
        }
    }
    wipe(k2, sizeof k2);
    return true;
}

void hal_aes_get_stats(hal_aes_stats_t *out) { *out = stats; }
