/* SPDX-License-Identifier: MIT
 * CC2652P AES-128 accelerator: single-block ECB encryption for CCM*, and
 * native AES-CCM* (one pass: AAD then data by DMA, tag read from the engine),
 * after the sequence of TI's AESCCMCC26XX driver (polling mode).
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
#include "ccm_star.h"

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
static bool engine_key_selected;
/* Word aligned DMA buffers in SRAM; the key buffer is wiped after use. */
static uint32_t dma_in[4], dma_out[4], dma_key[4];
/* Native CCM*: AAD, data in, data out (word aligned, wiped after use). */
static uint32_t dma_aad[HAL_AES_CCM_MAX / 4], dma_data[HAL_AES_CCM_MAX / 4], dma_res[HAL_AES_CCM_MAX / 4];
/* Key check: software E(key, 0^128) of the loaded key, computed once per load. */
static uint8_t kat_ref[16];
static bool kat_ready;
/* The key check runs at every key load, after any error (kat_ready reset by
 * recover), and then once every HAL_AES_KEY_CHECK_EVERY operations with the
 * same key: a key silently lost in between is caught by the backend's
 * re-verification of every rejected frame and by its random audits. */
#define HAL_AES_KEY_CHECK_EVERY 16u
static uint32_t ops_since_check;
static hal_aes_stats_t stats;
static volatile uint32_t busy;

static bool try_lock(void) { return __atomic_exchange_n(&busy, 1u, __ATOMIC_ACQUIRE) == 0u; }
static void unlock(void) { __atomic_store_n(&busy, 0u, __ATOMIC_RELEASE); }

static void wipe(void *p, unsigned n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

static inline void copy_to_words(uint32_t *dst, const uint8_t *src, unsigned len) {
    unsigned w = len / 4u;
    unsigned rem = len & 3u;
    const uint8_t *s = src;
    for (unsigned i = 0; i < w; i++) {
        dst[i] = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
        s += 4;
    }
    if (rem) {
        uint32_t last = 0;
        for (unsigned i = 0; i < rem; i++) last |= (uint32_t)s[i] << (8 * i);
        dst[w] = last;
    }
}

static inline void copy_from_words(uint8_t *dst, const uint32_t *src, unsigned len) {
    unsigned w = len / 4u;
    unsigned rem = len & 3u;
    uint8_t *d = dst;
    for (unsigned i = 0; i < w; i++) {
        uint32_t v = src[i];
        d[0] = (uint8_t)v;
        d[1] = (uint8_t)(v >> 8);
        d[2] = (uint8_t)(v >> 16);
        d[3] = (uint8_t)(v >> 24);
        d += 4;
    }
    if (rem) {
        uint32_t v = src[w];
        for (unsigned i = 0; i < rem; i++) d[i] = (uint8_t)(v >> (8 * i));
    }
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
    kat_ready = false;
    engine_key_selected = false;
    wipe(cached_key, sizeof cached_key);
    wipe(kat_ref, sizeof kat_ref);
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
    if (!(RD(CRYPTO_O_KEYWRITTENAREA) & (1u << KEY_AREA))) {
        engine_key_selected = false;
        stats.key_errors++;
        return false;
    }
    if (engine_key_selected) return true;
    WR(CRYPTO_O_KEYREADAREA, KEY_AREA);
    if (!wait_reg(CRYPTO_O_KEYREADAREA, CRYPTO_KEYREADAREA_BUSY_M, 0)) { stats.timeouts++; return false; }
    if (RD(CRYPTO_O_IRQSTAT) & CRYPTO_IRQSTAT_KEY_ST_RD_ERR_M) { stats.key_errors++; return false; }
    engine_key_selected = true;
    return true;
}

/* Make `key` the key of key store area 6 (written only when it changed). */
static bool load_key(const uint8_t key[16]) {
    bool same = key_loaded;
    for (unsigned i = 0; i < 16 && same; i++) same = cached_key[i] == key[i];
    if (same) return true;
    key_loaded = false;
    kat_ready = false;
    engine_key_selected = false;
    if (!key_write(key)) return false;
    for (unsigned i = 0; i < 16; i++) cached_key[i] = key[i];
    key_loaded = true;
    stats.key_loads++;
    return true;
}

/* One ECB block with the key of area 6. */
static bool ecb_locked(const uint8_t in[16], uint8_t out[16]) {
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

static bool block_locked(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    return load_key(key) && ecb_locked(in, out);
}

/* The engine must hold `key`: E(key, 0^128) by the engine == by software.
 * Catches a stale or foreign key (e.g. the well-known link key left in the
 * engine) before it could authenticate a forged frame. */
static const uint8_t zero_block[16];
static bool key_check(const uint8_t key[16]) {
    if (!kat_ready) {
        aes128_ctx_t a;
        aes128_init(&a, key);
        aes128_encrypt(&a, zero_block, kat_ref);
        aes128_clear(&a);
        kat_ready = true;
    }
    uint32_t chk[4];
    if (!ecb_locked(zero_block, (uint8_t *)chk)) return false;
    const uint32_t *ref = (const uint32_t *)(const void *)kat_ref;
    uint32_t diff = (chk[0] ^ ref[0]) | (chk[1] ^ ref[1]) | (chk[2] ^ ref[2]) | (chk[3] ^ ref[3]);
    volatile uint32_t *v = chk;
    v[0] = 0; v[1] = 0; v[2] = 0; v[3] = 0;
    if (diff) { stats.key_check_failures++; return false; }
    return true;
}

static bool ccm_locked(const uint8_t key[16], bool encrypt, const uint8_t nonce[13], const uint8_t *aad,
                       unsigned aad_len, const uint8_t *in, unsigned len, uint8_t *out, uint8_t tag[4]) {
    uint32_t t0 = CYCLES();
    if (!load_key(key)) return false;
    if (!kat_ready || ++ops_since_check >= HAL_AES_KEY_CHECK_EVERY) {
        if (!key_check(key)) return false;
        ops_since_check = 0;
    }
    WR(CRYPTO_O_IRQCLR, CRYPTO_IRQCLR_RESULT_AVAIL | CRYPTO_IRQCLR_DMA_IN_DONE | CRYPTO_IRQSTAT_DMA_BUS_ERR_M);
    WR(CRYPTO_O_IRQEN, CRYPTO_IRQEN_DMA_IN_DONE_M | CRYPTO_IRQEN_RESULT_AVAIL_M);
    WR(CRYPTO_O_ALGSEL, CRYPTO_ALGSEL_AES_M);
    if (!key_select()) return false;
    /* AESWriteCCMInitializationVector: flags L - 1 = 1, nonce, counter 0. */
    WR(CRYPTO_O_AESIV0, 1u | ((uint32_t)nonce[0] << 8) | ((uint32_t)nonce[1] << 16) | ((uint32_t)nonce[2] << 24));
    WR(CRYPTO_O_AESIV1, (uint32_t)nonce[3] | ((uint32_t)nonce[4] << 8) | ((uint32_t)nonce[5] << 16) | ((uint32_t)nonce[6] << 24));
    WR(CRYPTO_O_AESIV2, (uint32_t)nonce[7] | ((uint32_t)nonce[8] << 8) | ((uint32_t)nonce[9] << 16) | ((uint32_t)nonce[10] << 24));
    WR(CRYPTO_O_AESIV3, (uint32_t)nonce[11] | ((uint32_t)nonce[12] << 8));
    /* AESConfigureCCMCtrl(13, 4, encrypt): L field 1 (L = 2), M field 1 (M = 4). */
    WR(CRYPTO_O_AESCTL, (1u << CRYPTO_AESCTL_CCM_L_S) | (1u << CRYPTO_AESCTL_CCM_M_S) | CRYPTO_AESCTL_CCM |
                            CRYPTO_AESCTL_CTR | CRYPTO_AESCTL_SAVE_CONTEXT | CRYPTO_AESCTL_CTR_WIDTH_128_BIT |
                            (encrypt ? CRYPTO_AESCTL_DIR : 0u));
    WR(CRYPTO_O_AESDATALEN0, len);
    WR(CRYPTO_O_AESDATALEN1, 0);
    WR(CRYPTO_O_AESAUTHLEN, aad_len);
    copy_to_words(dma_aad, aad, aad_len);
    uint32_t got = 0;
    uint32_t t1 = CYCLES();
    if (!start_dma(dma_aad, aad_len, 0, 0)) { stats.timeouts++; return false; }
    /* The data is copied while the engine authenticates the AAD. */
    copy_to_words(dma_data, in, len);
    if (!wait_flags(CRYPTO_IRQSTAT_DMA_IN_DONE_M, &got)) { stats.timeouts++; return false; }
    if (got & ERR_FLAGS) { stats.dma_errors++; return false; }
    uint32_t t2 = CYCLES();
    if (!start_dma(dma_data, len, dma_res, len)) { stats.timeouts++; return false; }
    if (!wait_flags(CRYPTO_IRQSTAT_RESULT_AVAIL_M, &got)) { stats.timeouts++; return false; }
    if (got & ERR_FLAGS) { stats.dma_errors++; return false; }
    if ((RD(CRYPTO_O_DMASTAT) & CRYPTO_DMASTAT_CH1_ACT) &&
        !wait_reg(CRYPTO_O_DMASTAT, CRYPTO_DMASTAT_CH1_ACT, 0)) { stats.timeouts++; return false; }
    uint32_t t3 = CYCLES();
    /* AESReadTag: the tag is valid once the context is saved. */
    if (((RD(CRYPTO_O_AESCTL) & CRYPTO_AESCTL_SAVED_CONTEXT_RDY_M) == 0) &&
        !wait_reg(CRYPTO_O_AESCTL, CRYPTO_AESCTL_SAVED_CONTEXT_RDY_M, CRYPTO_AESCTL_SAVED_CONTEXT_RDY_M)) {
        stats.timeouts++;
        return false;
    }
    uint32_t t0_tag = RD(CRYPTO_O_AESTAGOUT0);
    WR(CRYPTO_O_ALGSEL, 0);
    tag[0] = (uint8_t)t0_tag;
    tag[1] = (uint8_t)(t0_tag >> 8);
    tag[2] = (uint8_t)(t0_tag >> 16);
    tag[3] = (uint8_t)(t0_tag >> 24);
    copy_from_words(out, dma_res, len);
    stats.ccm_ops++;
    uint32_t t4 = CYCLES();
    stats.prof_setup += t1 - t0;
    stats.prof_aad += t2 - t1;
    stats.prof_data += t3 - t2;
    stats.prof_tail += t4 - t3;
    return true;
}

/* Only what the operation used (bounded by the buffers): the rest is zero. */
static void wipe_words(uint32_t *p, unsigned bytes, unsigned cap_words) {
    volatile uint32_t *v = p;
    unsigned n = (bytes + 3u) / 4u;
    if (n > cap_words) n = cap_words;
    while (n--) *v++ = 0;
}
static void wipe_ccm_buffers(unsigned aad_len, unsigned len) {
    wipe_words(dma_aad, aad_len, HAL_AES_CCM_MAX / 4u);
    wipe_words(dma_data, len, HAL_AES_CCM_MAX / 4u);
    wipe_words(dma_res, len, HAL_AES_CCM_MAX / 4u);
}

bool hal_aes_ccm(const uint8_t key[16], bool encrypt, const uint8_t nonce[13], const uint8_t *aad, unsigned aad_len,
                 const uint8_t *in, unsigned len, uint8_t *out, uint8_t tag[4]) {
    if (!aad_len || aad_len > HAL_AES_CCM_MAX || !len || len > HAL_AES_CCM_MAX) return false;
    if (!try_lock()) { stats.busy++; return false; }
    bool ok = ccm_locked(key, encrypt, nonce, aad, aad_len, in, len, out, tag);
    if (!ok) {
        recover();
        wipe(dma_in, sizeof dma_in);
        wipe(dma_out, sizeof dma_out);
    }
    wipe_ccm_buffers(aad_len, len);
    unlock();
    return ok;
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
    kat_ready = false;
    engine_key_selected = false;
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

/* Native CCM* against the software CCM*: seal and open, lengths around the
 * block boundaries, two alternating keys (reload + key check), modified
 * ciphertext. */
bool hal_aes_ccm_selftest(void) {
    static const uint8_t lens[][2] = {{1, 1}, {13, 15}, {22, 16}, {22, 17}, {30, 40}, {45, 82}, {16, 128}, {128, 3}};
    static uint8_t aad[HAL_AES_CCM_MAX], pt[HAL_AES_CCM_MAX], ref[HAL_AES_CCM_MAX], got[HAL_AES_CCM_MAX];
    uint8_t key[16], nonce[13], ref_mic[4], tag[4];
    uint32_t x = CYCLES() | 1u;
    bool ok = true;
    for (unsigned k = 0; k < sizeof lens / sizeof lens[0] && ok; k++) {
        for (unsigned i = 0; i < HAL_AES_CCM_MAX; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            aad[i] = (uint8_t)x;
            pt[i] = (uint8_t)(x >> 8);
            if (i < 16) key[i] = (uint8_t)((k & 1u) ? (x >> 16) : i);   /* two keys alternate */
            if (i < 13) nonce[i] = (uint8_t)(x >> 24);
        }
        unsigned al = lens[k][0], dl = lens[k][1];
        aes128_ctx_t a;
        aes128_init(&a, key);
        ok = ccm_star_seal(&a, nonce, aad, al, pt, dl, ref, ref_mic, 4, true);
        aes128_clear(&a);
        /* seal */
        ok = ok && hal_aes_ccm(key, true, nonce, aad, al, pt, dl, got, tag);
        for (unsigned i = 0; ok && i < dl; i++) ok = got[i] == ref[i];
        for (unsigned i = 0; ok && i < 4; i++) ok = tag[i] == ref_mic[i];
        /* open: plaintext back, and the engine's MIC equals the frame's */
        ok = ok && hal_aes_ccm(key, false, nonce, aad, al, ref, dl, got, tag);
        for (unsigned i = 0; ok && i < dl; i++) ok = got[i] == pt[i];
        for (unsigned i = 0; ok && i < 4; i++) ok = tag[i] == ref_mic[i];
        /* a modified ciphertext gives another MIC */
        ref[0] ^= 0x01u;
        ok = ok && hal_aes_ccm(key, false, nonce, aad, al, ref, dl, got, tag);
        uint8_t same = 1;
        for (unsigned i = 0; i < 4; i++) same &= (uint8_t)(tag[i] == ref_mic[i]);
        ok = ok && !same;
    }
    wipe(key, sizeof key);
    wipe(got, sizeof got);
    wipe(pt, sizeof pt);
    return ok;
}

void hal_aes_get_stats(hal_aes_stats_t *out) { *out = stats; }
