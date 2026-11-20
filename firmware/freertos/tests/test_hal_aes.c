/* SPDX-License-Identifier: MIT
 * hal_aes.c (the CC2652P accelerator driver) executed on the host against a
 * register model of the crypto peripheral, with injected faults: DMA bus
 * error, key store write/read errors, engine that never completes, stuck
 * key-read busy, stuck DMA, stale IRQ flags. Checks that every wait is
 * bounded, that each error is reported (never a wrong block returned as
 * good), that the engine is reset and the key reloaded afterwards, and that
 * the CCM* backend on top of it stays bit-exact with the software path.
 * The native CCM* mode is modelled too (one pass, AAD then data, tag
 * registers), with its own faults: stale key in the engine, tag registers not
 * updated, context never saved, AAD DMA never done, silently corrupted output.
 *
 * The model follows the TI register description (hw_crypto.h) and TI's
 * driver sequences; it is NOT the silicon. Behaviour on the MCU remains to
 * be measured (docs/firmware/hardware-campaign.md).
 */
#define HAL_AES_HOST_MODEL 1
#include "../hal/hal_aes.c"

#include "frt_test.h"
#include "ccm_star.h"
#include "zb_ccm_backend.h"

/* ---- model ---------------------------------------------------------------- */
enum {
    M_OK, M_DMA_BUS_ERR, M_NEVER_DONE, M_KEY_WR_ERR, M_KEY_RD_ERR, M_KEY_RD_STUCK,
    M_DMA_STUCK, M_IRQ_STUCK, M_KEY_NOT_WRITTEN, M_OUT_DMA_LATE,
    /* native CCM* */
    M_STALE_KEY, M_TAG_STALE, M_CTX_NEVER, M_AAD_NEVER, M_FLIP_OUT, M_KEY_SWAP
};
static struct {
    uint32_t reg[0x800 / 4];
    uint8_t keystore[8][16];
    uint8_t engine_key[16];
    bool engine_key_valid;
    const void *ptr[64];
    unsigned nptr;
    uint32_t cycles;
    int fault;          /* applied to the next matching operation */
    unsigned fault_after; /* ... after this many successful AES blocks */
    unsigned aes_blocks, key_writes, sw_resets, dma_resets;
    bool dma_active, out_late;
    uint8_t aad[256];
    uint32_t aad_len;     /* AAD consumed by the current CCM operation */
    bool flip_always;     /* M_FLIP_OUT on every CCM operation (persistent fault) */
    unsigned ccm_ops;
} m;

#define REG(off) m.reg[(off) / 4]

uint32_t crypto_model_cycles(void) { return m.cycles += 97; }

uint32_t crypto_model_addr(const void *p) {
    for (unsigned i = 0; i < m.nptr; i++) if (m.ptr[i] == p) return 0x20000000u + i;
    m.ptr[m.nptr] = p;
    return 0x20000000u + m.nptr++;
}
static void *ptr_of(uint32_t h) { return (void *)(uintptr_t)m.ptr[h - 0x20000000u]; }

static bool fault_now(int kind) {
    if (m.fault != kind) return false;
    if (m.fault_after) { m.fault_after--; return false; }
    return true;
}

uint32_t crypto_rd(uint32_t off) {
    if (off == CRYPTO_O_DMASTAT) {
        if (m.out_late) { m.out_late = false; return CRYPTO_DMASTAT_CH1_ACT; }   /* once */
        return m.dma_active ? (CRYPTO_DMASTAT_CH0_ACT | CRYPTO_DMASTAT_CH1_ACT) : 0;
    }
    if (off == CRYPTO_O_IRQSTAT && m.fault == M_IRQ_STUCK) return REG(off) | CRYPTO_IRQSTAT_RESULT_AVAIL_M;
    return REG(off);
}

/* Native CCM* of the engine: correct only when programmed as TI's
 * AESCCMCC26XX does (L = 2, M = 4, IV = L-1 | nonce | 0, lengths, AAD first). */
static void ccm_op(void) {
    if (fault_now(M_DMA_BUS_ERR)) { REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_DMA_BUS_ERR_M | CRYPTO_IRQSTAT_RESULT_AVAIL_M; return; }
    if (fault_now(M_DMA_STUCK)) { m.dma_active = true; REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_RESULT_AVAIL_M; return; }
    const uint8_t *in = ptr_of(REG(CRYPTO_O_DMACH0EXTADDR));
    uint8_t *out = ptr_of(REG(CRYPTO_O_DMACH1EXTADDR));
    uint32_t len = REG(CRYPTO_O_DMACH0LEN), ctl = REG(CRYPTO_O_AESCTL);
    bool encrypt = (ctl & CRYPTO_AESCTL_DIR) != 0;
    const uint32_t want = (1u << CRYPTO_AESCTL_CCM_L_S) | (1u << CRYPTO_AESCTL_CCM_M_S) | CRYPTO_AESCTL_CCM |
                          CRYPTO_AESCTL_CTR | CRYPTO_AESCTL_SAVE_CONTEXT | CRYPTO_AESCTL_CTR_WIDTH_128_BIT |
                          (encrypt ? CRYPTO_AESCTL_DIR : 0u);
    uint8_t iv[16];
    for (unsigned w = 0; w < 4; w++)
        for (unsigned b = 0; b < 4; b++) iv[4 * w + b] = (uint8_t)(REG(CRYPTO_O_AESIV0 + 4u * w) >> (8 * b));
    uint8_t res[256] = {0}, tag[4] = {0};
    if (m.engine_key_valid && REG(CRYPTO_O_ALGSEL) == CRYPTO_ALGSEL_AES_M && ctl == want && iv[0] == 1 && !iv[14] &&
        !iv[15] && REG(CRYPTO_O_AESDATALEN0) == len && REG(CRYPTO_O_DMACH1LEN) == len && !REG(CRYPTO_O_AESDATALEN1) &&
        REG(CRYPTO_O_AESAUTHLEN) == m.aad_len && m.aad_len && len && len <= sizeof res) {
        aes128_ctx_t a;
        aes128_init(&a, m.engine_key);
        if (encrypt) {
            (void)ccm_star_seal(&a, iv + 1, m.aad, m.aad_len, in, len, res, tag, 4, true);
        } else {
            uint8_t ignored[4];
            /* CTR is its own inverse: plaintext, then the MIC of that plaintext */
            (void)ccm_star_seal(&a, iv + 1, m.aad, m.aad_len, in, len, res, ignored, 4, true);
            (void)ccm_star_seal(&a, iv + 1, m.aad, m.aad_len, res, len, ignored, tag, 4, true);
        }
        aes128_clear(&a);
    }
    if (fault_now(M_FLIP_OUT) || m.flip_always) res[0] ^= 0x01u;   /* silent: nothing reported */
    if (fault_now(M_KEY_SWAP)) {   /* engine key silently replaced, for the next operations */
        for (unsigned i = 0; i < 16; i++) m.engine_key[i] ^= 0xA5u;
        for (unsigned i = 0; i < 8; i++)
            for (unsigned k = 0; k < 16; k++) m.keystore[i][k] ^= 0xA5u;
    }
    for (uint32_t i = 0; i < len && i < sizeof res; i++) out[i] = res[i];
    if (!fault_now(M_TAG_STALE))
        REG(CRYPTO_O_AESTAGOUT0) = (uint32_t)tag[0] | (uint32_t)tag[1] << 8 | (uint32_t)tag[2] << 16 | (uint32_t)tag[3] << 24;
    if (!fault_now(M_CTX_NEVER)) REG(CRYPTO_O_AESCTL) |= CRYPTO_AESCTL_SAVED_CONTEXT_RDY_M;
    m.ccm_ops++;
    REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_DMA_IN_DONE_M | CRYPTO_IRQSTAT_RESULT_AVAIL_M;
}

static void aes_op(void) {
    if (fault_now(M_NEVER_DONE)) return;
    if (fault_now(M_DMA_BUS_ERR)) { REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_DMA_BUS_ERR_M | CRYPTO_IRQSTAT_RESULT_AVAIL_M; return; }
    if (fault_now(M_DMA_STUCK)) { m.dma_active = true; REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_RESULT_AVAIL_M; return; }
    if (fault_now(M_OUT_DMA_LATE)) m.out_late = true;
    if (REG(CRYPTO_O_AESCTL) & CRYPTO_AESCTL_CCM) { ccm_op(); return; }
    const uint8_t *in = ptr_of(REG(CRYPTO_O_DMACH0EXTADDR));
    uint8_t *out = ptr_of(REG(CRYPTO_O_DMACH1EXTADDR));
    uint8_t res[16] = {0};
    /* The engine only produces a correct block when programmed as TI does. */
    if (m.engine_key_valid && REG(CRYPTO_O_ALGSEL) == CRYPTO_ALGSEL_AES_M && REG(CRYPTO_O_AESCTL) == CRYPTO_AESCTL_DIR &&
        REG(CRYPTO_O_AESDATALEN0) == 16 && REG(CRYPTO_O_DMACH0LEN) == 16 && REG(CRYPTO_O_DMACH1LEN) == 16) {
        aes128_ctx_t a;
        aes128_init(&a, m.engine_key);
        aes128_encrypt(&a, in, res);
        aes128_clear(&a);
    }
    for (unsigned i = 0; i < 16; i++) out[i] = res[i];
    m.aes_blocks++;
    REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_DMA_IN_DONE_M | CRYPTO_IRQSTAT_RESULT_AVAIL_M;
}

void crypto_wr(uint32_t off, uint32_t v) {
    switch (off) {
    case CRYPTO_O_IRQCLR: REG(CRYPTO_O_IRQSTAT) &= ~v; return;
    case CRYPTO_O_KEYWRITTENAREA: REG(off) &= ~v; return;   /* write 1 to invalidate */
    case CRYPTO_O_SWRESET:
        m.sw_resets++;
        m.engine_key_valid = false;
        REG(CRYPTO_O_ALGSEL) = REG(CRYPTO_O_AESCTL) = 0;
        if (m.fault == M_NEVER_DONE || m.fault == M_DMA_BUS_ERR || m.fault == M_KEY_RD_STUCK) m.fault = M_OK;
        return;
    case CRYPTO_O_DMASWRESET:
        m.dma_resets++;
        if (m.fault != M_DMA_STUCK) m.dma_active = false;
        return;
    case CRYPTO_O_KEYREADAREA:
        REG(off) = v;
        if (fault_now(M_KEY_RD_STUCK)) { REG(off) |= CRYPTO_KEYREADAREA_BUSY_M; return; }
        if (fault_now(M_KEY_RD_ERR)) { REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_KEY_ST_RD_ERR_M; return; }
        if (fault_now(M_STALE_KEY)) return;   /* engine keeps its previous key */
        if (v < 8 && (REG(CRYPTO_O_KEYWRITTENAREA) & (1u << v))) {
            for (unsigned i = 0; i < 16; i++) m.engine_key[i] = m.keystore[v][i];
            m.engine_key_valid = true;
        }
        return;
    case CRYPTO_O_AESCTL:
        REG(off) = v;
        m.aad_len = 0;   /* a new configuration starts a new operation */
        return;
    case CRYPTO_O_DMACH0LEN:
        REG(off) = v;
        if (REG(CRYPTO_O_ALGSEL) == CRYPTO_ALGSEL_AES_M && (REG(CRYPTO_O_AESCTL) & CRYPTO_AESCTL_CCM) && !m.aad_len &&
            REG(CRYPTO_O_AESAUTHLEN)) {
            /* CCM AAD phase (input channel only). */
            if (fault_now(M_AAD_NEVER)) return;
            const uint8_t *a = ptr_of(REG(CRYPTO_O_DMACH0EXTADDR));
            for (unsigned i = 0; i < v && i < sizeof m.aad; i++) m.aad[i] = a[i];
            m.aad_len = v;
            REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_DMA_IN_DONE_M;
            return;
        }
        if (REG(CRYPTO_O_ALGSEL) == CRYPTO_ALGSEL_KEY_STORE) {
            m.key_writes++;
            if (fault_now(M_NEVER_DONE)) return;
            if (fault_now(M_KEY_WR_ERR)) { REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_KEY_ST_WR_ERR_M; return; }
            uint32_t area = REG(CRYPTO_O_KEYWRITEAREA);
            for (unsigned a = 0; a < 8; a++)
                if (area == (1u << a)) {
                    const uint8_t *k = ptr_of(REG(CRYPTO_O_DMACH0EXTADDR));
                    for (unsigned i = 0; i < 16; i++) m.keystore[a][i] = k[i];
                    if (!fault_now(M_KEY_NOT_WRITTEN)) REG(CRYPTO_O_KEYWRITTENAREA) |= area;
                }
            REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_DMA_IN_DONE_M | CRYPTO_IRQSTAT_RESULT_AVAIL_M;
        }
        return;
    case CRYPTO_O_DMACH1LEN:
        REG(off) = v;
        aes_op();
        return;
    default: REG(off) = v; return;
    }
}

/* ---- tests ---------------------------------------------------------------- */
static void sw_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    aes128_ctx_t a;
    aes128_init(&a, key);
    aes128_encrypt(&a, in, out);
    aes128_clear(&a);
}

static void fresh_model(void) {
    memset(&m, 0, sizeof m);
    key_loaded = false;          /* driver state (static in hal_aes.c) */
    kat_ready = false;
    ops_since_check = 0;
    memset(&stats, 0, sizeof stats);
}

static void driver_checks(void) {
    hal_aes_stats_t st;
    fresh_model();
    CHECK(hal_aes_selftest());
    hal_aes_get_stats(&st);
    CHECK(st.blocks == 17 && st.key_loads == 9 && st.resets == 0);   /* cached key used */
    CHECK(REG(CRYPTO_O_ALGSEL) == 0);                                 /* engine left idle */
    for (unsigned i = 0; i < 4; i++) CHECK(dma_key[i] == 0);          /* key copy wiped */

    /* Each fault: reported, bounded, engine reset, next block correct. */
    static const struct { int fault; const char *name; } faults[] = {
        {M_DMA_BUS_ERR, "dma bus error"}, {M_NEVER_DONE, "engine never completes"},
        {M_KEY_WR_ERR, "key store write error"}, {M_KEY_RD_ERR, "key store read error"},
        {M_KEY_RD_STUCK, "key read busy stuck"}, {M_KEY_NOT_WRITTEN, "key area not marked written"},
        {M_IRQ_STUCK, "stale result flag"}, {M_OUT_DMA_LATE, "output DMA finishing late"},
        {M_DMA_STUCK, "DMA stuck active"},
    };
    for (unsigned k = 0; k < sizeof faults / sizeof faults[0]; k++) {
        for (unsigned when = 0; when < 3; when++) {
            fresh_model();
            uint8_t key[16], blk[16], out[16], ref[16];
            for (unsigned i = 0; i < 16; i++) { key[i] = (uint8_t)frt_rand(); blk[i] = (uint8_t)frt_rand(); }
            sw_block(key, blk, ref);
            for (unsigned i = 0; i < when; i++) CHECK(hal_aes_block(key, blk, out) && !memcmp(out, ref, 16));
            if (when == 2) for (unsigned i = 0; i < 16; i++) key[i] ^= 0x5A;   /* new key: write path */
            sw_block(key, blk, ref);
            m.fault = faults[k].fault;
            uint32_t c0 = m.cycles;
            memset(out, 0xEE, sizeof out);
            bool ok = hal_aes_block(key, blk, out);
            uint32_t spent = m.cycles - c0;
            hal_aes_get_stats(&st);
            if (faults[k].fault == M_OUT_DMA_LATE) {
                /* not an error: the driver waits for the output channel */
                CHECK(ok && !memcmp(out, ref, 16));
                continue;
            }
            /* Key write faults only exist when the key is (re)loaded. */
            bool cached = when == 1;
            bool expect_fail = !(cached && (faults[k].fault == M_KEY_WR_ERR || faults[k].fault == M_KEY_NOT_WRITTEN));
            CHECK(ok == !expect_fail);
            if (ok) { CHECK(!memcmp(out, ref, 16)); continue; }
            CHECK(st.resets == 1);
            CHECK(spent < 4u * WAIT_BUDGET_CYCLES);                 /* bounded */
            CHECK(!key_loaded);
            if (faults[k].fault == M_DMA_STUCK) CHECK(st.reset_timeouts == 1);
            m.fault = M_OK;
            m.dma_active = false;
            uint32_t loads = st.key_loads;
            CHECK(hal_aes_block(key, blk, out) && !memcmp(out, ref, 16));   /* recovered */
            hal_aes_get_stats(&st);
            CHECK(st.key_loads == loads + 1);                                /* key reloaded */
        }
        printf("hal_aes: fault '%s' handled\n", faults[k].name);
    }

    /* Key switch: the new key is used immediately (no stale cache). */
    fresh_model();
    uint8_t k1[16] = {1}, k2[16] = {2}, blk[16] = {3}, out[16], ref[16];
    CHECK(hal_aes_block(k1, blk, out));
    sw_block(k2, blk, ref);
    CHECK(hal_aes_block(k2, blk, out) && !memcmp(out, ref, 16));
    /* Invalidation (e.g. after a power-down) forces a reload. */
    hal_aes_get_stats(&st);
    uint32_t loads = st.key_loads;
    hal_aes_invalidate();
    CHECK(hal_aes_block(k2, blk, out) && !memcmp(out, ref, 16));
    hal_aes_get_stats(&st);
    CHECK(st.key_loads == loads + 1);
    /* Key store area lost behind the driver's back (no invalidate call):
     * reported as an error, never a block with another key. */
    REG(CRYPTO_O_KEYWRITTENAREA) = 0;
    CHECK(!hal_aes_block(k2, blk, out));
    CHECK(hal_aes_block(k2, blk, out) && !memcmp(out, ref, 16));

    /* Busy: a second caller is refused at once (it uses software). */
    busy = 1;
    CHECK(!hal_aes_block(k2, blk, out));
    busy = 0;
    hal_aes_get_stats(&st);
    CHECK(st.busy == 1);
}

static void backend_checks(void) {
    /* The CCM* hardware backend on top of the modelled driver: bit-exact,
     * and faults injected in the middle of frames are absorbed. */
    fresh_model();
    zb_ccm_backend_set_block(hal_aes_block);
    unsigned injected = 0;
    for (unsigned round = 0; round < 600; round++) {
        uint8_t key[16], nonce[13], aad[30], pt[127], c1[127], c2[127], m1[4], m2[4], buf[127];
        for (unsigned i = 0; i < 16; i++) key[i] = (uint8_t)(round < 300 ? i : frt_rand());   /* same key, then many */
        for (unsigned i = 0; i < 13; i++) nonce[i] = (uint8_t)frt_rand();
        size_t alen = frt_rand() % sizeof aad, plen = frt_rand() % sizeof pt;
        for (unsigned i = 0; i < alen; i++) aad[i] = (uint8_t)frt_rand();
        for (unsigned i = 0; i < plen; i++) pt[i] = (uint8_t)frt_rand();
        CHECK(zb_ccm_backend_sw(0, true, key, nonce, aad, alen, pt, plen, c1, m1));
        if (round % 5 == 4) {   /* one frame in five sees a fault on a random block */
            static const int kinds[] = {M_DMA_BUS_ERR, M_NEVER_DONE, M_KEY_RD_ERR, M_KEY_RD_STUCK};
            m.fault = kinds[frt_rand() % 4];
            m.fault_after = frt_rand() % 6;
            injected++;
        }
        memcpy(buf, pt, plen);
        CHECK(zb_ccm_backend_hw(0, true, key, nonce, aad, alen, buf, plen, buf, m2));
        CHECK(!memcmp(buf, c1, plen) && !memcmp(m1, m2, 4));
        CHECK(zb_ccm_backend_hw(0, false, key, nonce, aad, alen, buf, plen, buf, m1));
        CHECK(!memcmp(buf, pt, plen));
        m.fault = M_OK;
        m.fault_after = 0;
        (void)c2;
    }
    zb_ccm_hw_stats_t cs;
    zb_ccm_backend_hw_stats(&cs);
    hal_aes_stats_t st;
    hal_aes_get_stats(&st);
    CHECK(!cs.hw_disabled && cs.hw_mismatches == 0 && cs.hw_audit_failures == 0);
    CHECK(cs.hw_ops > 900 && cs.sw_redo > 0 && st.resets == cs.hw_faults);
    printf("hal_aes: CCM* over the modelled driver: %u ops on hardware, %u faults injected, %u redone in software, "
           "%u audits\n", (unsigned)cs.hw_ops, injected, (unsigned)cs.sw_redo, (unsigned)cs.hw_audits);
    zb_ccm_backend_set_block(0);
}

static void native_checks(void) {
    hal_aes_stats_t st;
    /* 1. Self-test of the native mode against the software CCM*. */
    fresh_model();
    CHECK(hal_aes_ccm_selftest());
    hal_aes_get_stats(&st);
    CHECK(st.ccm_ops == 24 && st.key_check_failures == 0 && st.resets == 0 && m.ccm_ops == 24);
    CHECK(REG(CRYPTO_O_ALGSEL) == 0);
    for (unsigned i = 0; i < HAL_AES_CCM_MAX / 4; i++) CHECK(!dma_aad[i] && !dma_data[i] && !dma_res[i]);

    /* 2. Each fault of the native path: reported, bounded, engine reset,
     *    next operation correct. */
    static const struct { int fault; const char *name; } faults[] = {
        {M_STALE_KEY, "stale key in the engine"}, {M_CTX_NEVER, "context never saved"},
        {M_AAD_NEVER, "AAD DMA never done"}, {M_NEVER_DONE, "engine never completes"},
        {M_DMA_BUS_ERR, "DMA bus error"}, {M_DMA_STUCK, "DMA stuck active"}, {M_KEY_RD_ERR, "key read error"},
    };
    for (unsigned k = 0; k < sizeof faults / sizeof faults[0]; k++) {
        fresh_model();
        uint8_t k1[16], k2[16], nonce[13], aad[22], pt[40], ref[40], out[40], mic[4], tag[4];
        for (unsigned i = 0; i < 16; i++) { k1[i] = (uint8_t)frt_rand(); k2[i] = (uint8_t)frt_rand(); }
        for (unsigned i = 0; i < 13; i++) nonce[i] = (uint8_t)frt_rand();
        for (unsigned i = 0; i < sizeof aad; i++) aad[i] = (uint8_t)frt_rand();
        for (unsigned i = 0; i < sizeof pt; i++) pt[i] = (uint8_t)frt_rand();
        CHECK(hal_aes_ccm(k1, true, nonce, aad, sizeof aad, pt, sizeof pt, out, tag));   /* k1 in the engine */
        CHECK(zb_ccm_backend_sw(0, true, k2, nonce, aad, sizeof aad, pt, sizeof pt, ref, mic));
        m.fault = faults[k].fault;
        uint32_t c0 = m.cycles;
        bool ok = hal_aes_ccm(k2, true, nonce, aad, sizeof aad, pt, sizeof pt, out, tag);
        uint32_t spent = m.cycles - c0;
        hal_aes_get_stats(&st);
        CHECK(!ok && st.resets == 1 && spent < 6u * WAIT_BUDGET_CYCLES);
        if (faults[k].fault == M_STALE_KEY) CHECK(st.key_check_failures == 1);   /* never ran with k1 */
        m.fault = M_OK;
        m.dma_active = false;
        CHECK(hal_aes_ccm(k2, true, nonce, aad, sizeof aad, pt, sizeof pt, out, tag));
        CHECK(!memcmp(out, ref, sizeof ref) && !memcmp(tag, mic, 4));
        printf("hal_aes: native CCM fault '%s' handled\n", faults[k].name);
    }
    /* Refused lengths and busy engine: false at once, nothing touched. */
    {
        uint8_t key[16] = {7}, nonce[13] = {0}, b[4] = {0}, tag[4];
        CHECK(!hal_aes_ccm(key, true, nonce, b, 0, b, 4, b, tag));
        CHECK(!hal_aes_ccm(key, true, nonce, b, 4, b, 0, b, tag));
        busy = 1;
        CHECK(!hal_aes_ccm(key, true, nonce, b, 4, b, 4, b, tag));
        busy = 0;
    }

    /* 3. CCM* backend on the native mode: bit-exact with software, faults
     *    absorbed (software redo), stale tags caught, forged MIC rejected. */
    fresh_model();
    zb_ccm_backend_set_block(hal_aes_block);
    zb_ccm_backend_set_native(hal_aes_ccm);
    unsigned injected = 0, forged = 0;
    for (unsigned round = 0; round < 600; round++) {
        uint8_t key[16], nonce[13], aad[30], pt[127], c1[127], m1[4], m2[4], buf[127];
        for (unsigned i = 0; i < 16; i++) key[i] = (uint8_t)(round < 300 ? i : frt_rand());
        for (unsigned i = 0; i < 13; i++) nonce[i] = (uint8_t)frt_rand();
        size_t alen = frt_rand() % sizeof aad, plen = frt_rand() % sizeof pt;   /* 0 lengths: block path */
        for (unsigned i = 0; i < alen; i++) aad[i] = (uint8_t)frt_rand();
        for (unsigned i = 0; i < plen; i++) pt[i] = (uint8_t)frt_rand();
        CHECK(zb_ccm_backend_sw(0, true, key, nonce, aad, alen, pt, plen, c1, m1));
        if (round % 5 == 4) {
            static const int kinds[] = {M_DMA_BUS_ERR, M_NEVER_DONE, M_STALE_KEY, M_CTX_NEVER, M_TAG_STALE, M_AAD_NEVER};
            m.fault = kinds[frt_rand() % 6];
            m.fault_after = frt_rand() % 3;
            injected++;
        }
        memcpy(buf, pt, plen);
        CHECK(zb_ccm_backend_hw(0, true, key, nonce, aad, alen, buf, plen, buf, m2));
        CHECK(!memcmp(buf, c1, plen) && !memcmp(m1, m2, 4));
        if (round % 7 == 3 && plen) {   /* forged MIC: rejected, output wiped */
            uint8_t bad[4];
            memcpy(bad, m1, 4);
            bad[1] ^= 0x40u;
            uint8_t copy[127];
            memcpy(copy, buf, plen);
            CHECK(!zb_ccm_backend_hw(0, false, key, nonce, aad, alen, copy, plen, copy, bad));
            for (size_t i = 0; i < plen; i++) CHECK(copy[i] == 0);
            forged++;
        }
        CHECK(zb_ccm_backend_hw(0, false, key, nonce, aad, alen, buf, plen, buf, m1));
        CHECK(!memcmp(buf, pt, plen));
        m.fault = M_OK;
        m.fault_after = 0;
    }
    zb_ccm_hw_stats_t cs;
    zb_ccm_backend_hw_stats(&cs);
    hal_aes_get_stats(&st);
    CHECK(!cs.hw_disabled && cs.hw_audit_failures == 0);
    CHECK(cs.native_ops > 900 && cs.sw_redo > 0 && cs.tag_suspect > 0 && st.key_check_failures > 0);
    printf("hal_aes: native CCM* backend: %u native ops, %u faults injected, %u redone in software "
           "(%u stale tags, %u key checks failed), %u forged MICs rejected, %u audits\n",
           (unsigned)cs.native_ops, injected, (unsigned)cs.sw_redo, (unsigned)cs.tag_suspect,
           (unsigned)st.key_check_failures, forged, (unsigned)cs.hw_audits);

    /* 3b. Engine key silently replaced while the driver's key cache still
     *     says "loaded": caught by the periodic key check within
     *     HAL_AES_KEY_CHECK_EVERY operations, the key reloaded, results right. */
    fresh_model();
    {
        uint8_t key[16] = {0x31}, nonce[13] = {0}, aad[8] = {1}, pt[24] = {2}, out[24], ref[24], tag[4], mic[4];
        CHECK(hal_aes_ccm(key, true, nonce, aad, sizeof aad, pt, sizeof pt, out, tag));
        m.fault = M_KEY_SWAP;
        CHECK(hal_aes_ccm(key, true, nonce, aad, sizeof aad, pt, sizeof pt, out, tag));   /* swap happens here */
        m.fault = M_OK;
        unsigned n = 0;
        hal_aes_get_stats(&st);
        while (st.key_check_failures == 0 && n < HAL_AES_KEY_CHECK_EVERY + 1u) {
            nonce[0] = (uint8_t)++n;
            (void)hal_aes_ccm(key, true, nonce, aad, sizeof aad, pt, sizeof pt, out, tag);
            hal_aes_get_stats(&st);
        }
        CHECK(st.key_check_failures == 1 && n <= HAL_AES_KEY_CHECK_EVERY);
        nonce[0] = 0x77;
        CHECK(zb_ccm_backend_sw(0, true, key, nonce, aad, sizeof aad, pt, sizeof pt, ref, mic));
        CHECK(hal_aes_ccm(key, true, nonce, aad, sizeof aad, pt, sizeof pt, out, tag));   /* reloaded */
        CHECK(!memcmp(out, ref, sizeof ref) && !memcmp(tag, mic, 4));
        printf("hal_aes: engine key silently replaced: detected after %u operations\n", n);
    }

    /* 4. Silent corruption of every output (nothing reported by the engine):
     *    the random audit catches it and abandons the accelerator. */
    fresh_model();
    zb_ccm_backend_set_block(hal_aes_block);
    zb_ccm_backend_set_native(hal_aes_ccm);
    m.flip_always = true;
    unsigned ops = 0;
    for (; ops < 200; ops++) {
        zb_ccm_backend_hw_stats(&cs);
        if (cs.hw_disabled) break;
        uint8_t key[16] = {9}, nonce[13], aad[16] = {1}, pt[32] = {2}, out[32], mic[4];
        for (unsigned i = 0; i < 13; i++) nonce[i] = (uint8_t)(ops + i);
        (void)zb_ccm_backend_hw(0, true, key, nonce, aad, sizeof aad, pt, sizeof pt, out, mic);
    }
    zb_ccm_backend_hw_stats(&cs);
    CHECK(cs.hw_disabled && cs.hw_audit_failures == 1 && ops <= 32u + 64u);   /* HW_AUDIT_MIN + 63, then the audited one */
    printf("hal_aes: silently corrupted output: accelerator abandoned after %u operations\n", ops);
    m.flip_always = false;
    zb_ccm_backend_set_native(0);
    zb_ccm_backend_set_block(0);
}

int main(void) {
    driver_checks();
    backend_checks();
    native_checks();
    PASS("hal_aes");
    return 0;
}
