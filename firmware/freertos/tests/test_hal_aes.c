/* SPDX-License-Identifier: MIT
 * hal_aes.c (the CC2652P accelerator driver) executed on the host against a
 * register model of the crypto peripheral, with injected faults: DMA bus
 * error, key store write/read errors, engine that never completes, stuck
 * key-read busy, stuck DMA, stale IRQ flags. Checks that every wait is
 * bounded, that each error is reported (never a wrong block returned as
 * good), that the engine is reset and the key reloaded afterwards, and that
 * the CCM* backend on top of it stays bit-exact with the software path.
 *
 * The model follows the TI register description (hw_crypto.h) and TI's
 * driver sequences; it is NOT the silicon. Behaviour on the MCU remains to
 * be measured (docs/freertos-migration/hardware-campaign.md).
 */
#define HAL_AES_HOST_MODEL 1
#include "../hal/hal_aes.c"

#include "frt_test.h"
#include "ccm_star.h"
#include "zb_ccm_backend.h"

/* ---- model ---------------------------------------------------------------- */
enum {
    M_OK, M_DMA_BUS_ERR, M_NEVER_DONE, M_KEY_WR_ERR, M_KEY_RD_ERR, M_KEY_RD_STUCK,
    M_DMA_STUCK, M_IRQ_STUCK, M_KEY_NOT_WRITTEN, M_OUT_DMA_LATE
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

static void aes_op(void) {
    if (fault_now(M_NEVER_DONE)) return;
    if (fault_now(M_DMA_BUS_ERR)) { REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_DMA_BUS_ERR_M | CRYPTO_IRQSTAT_RESULT_AVAIL_M; return; }
    if (fault_now(M_DMA_STUCK)) { m.dma_active = true; REG(CRYPTO_O_IRQSTAT) |= CRYPTO_IRQSTAT_RESULT_AVAIL_M; return; }
    if (fault_now(M_OUT_DMA_LATE)) m.out_late = true;
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
        if (v < 8 && (REG(CRYPTO_O_KEYWRITTENAREA) & (1u << v))) {
            for (unsigned i = 0; i < 16; i++) m.engine_key[i] = m.keystore[v][i];
            m.engine_key_valid = true;
        }
        return;
    case CRYPTO_O_DMACH0LEN:
        REG(off) = v;
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

int main(void) {
    driver_checks();
    backend_checks();
    PASS("hal_aes");
    return 0;
}
