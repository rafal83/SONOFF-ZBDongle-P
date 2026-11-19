/* SPDX-License-Identifier: MIT */
#include "zb_ccm_backend.h"
#include "aes128.h"
#include "ccm_star.h"

core_security_ccm_t zb_ccm_backend_active = zb_ccm_backend_sw;

bool zb_ccm_backend_sw(void *context, bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
                       const uint8_t *aad, size_t aad_len, const uint8_t *input, size_t input_len,
                       uint8_t *output, uint8_t mic[4]) {
    (void)context;
    aes128_ctx_t aes;
    aes128_init(&aes, key);
    bool ok = encrypt
        ? ccm_star_seal(&aes, nonce, aad, aad_len, input, input_len, output, mic, 4, true)
        : ccm_star_open(&aes, nonce, aad, aad_len, input, input_len, output, mic, 4, true);
    aes128_clear(&aes);
    return ok;
}

/* ---- hardware block cipher (installed by the platform) -----------------
 *
 * Safety rules (audit of 19 November):
 *  - the hardware never decides alone: an operation in which any block
 *    failed, or whose first block differs from the software AES, is redone
 *    entirely in software from an intact copy of the input. Nothing a faulty
 *    block produced is ever sent, and a frame is only accepted on a MIC that
 *    the software path would also accept;
 *  - in-place operations (input == output, the NWK/APS case) work on a copy,
 *    so a fault in the middle neither corrupts a frame being sealed nor loses
 *    a frame being opened (ccm_star_open_with wipes its output on failure);
 *  - the first block of EVERY operation is checked against the software AES:
 *    an accelerator returning wrong blocks without reporting an error (stale
 *    key, stuck output, all zeros -> a forged MIC of 0 would pass) is caught
 *    on the first operation and disabled for good;
 *  - every frame the accelerator REJECTS is re-verified in software (cost only
 *    on rejection, i.e. corrupted or forged traffic, never above the
 *    software-only path): a silent fault never drops a genuine frame and is
 *    detected on the first such frame;
 *  - one operation every 32..95 (pseudo-random, so that the audit cannot
 *    line up with a periodic traffic pattern) is entirely recomputed in
 *    software and compared (output, MIC, verdict): an intermittent silent
 *    fault on a later block of a SEALED frame (which would only produce a
 *    frame the receiver rejects) is detected and the accelerator disabled;
 *  - after HW_FAULT_LIMIT consecutive faulty operations the accelerator is
 *    disabled too (software only until reboot), visible in the statistics;
 *  - one operation at a time: a concurrent caller takes the software path
 *    instead of sharing the state.
 */
#define HW_MAX_INPUT 128u
#define HW_FAULT_LIMIT 8u
#define HW_AUDIT_MIN 32u     /* audit interval: HW_AUDIT_MIN + (0..63) operations */

static zb_aes_block_fn hw_block;
static zb_ccm_hw_stats_t hw_stats;
static uint32_t hw_consecutive_faults;
static volatile uint32_t hw_busy;
static uint32_t hw_audit_count, hw_audit_next = HW_AUDIT_MIN, hw_audit_rng = 0x9E3779B9u;
/* Software round keys of the last key, for the first-block check. */
static aes128_ctx_t verify_aes;
static uint8_t verify_key[16];
static bool verify_ready;

void zb_ccm_backend_set_block(zb_aes_block_fn fn) {
    hw_block = fn;
    hw_stats = (zb_ccm_hw_stats_t){0};   /* statistics of this installation */
    hw_consecutive_faults = 0;
    hw_audit_count = 0;
    hw_audit_next = HW_AUDIT_MIN;
}

void zb_ccm_backend_hw_stats(zb_ccm_hw_stats_t *out) { *out = hw_stats; }

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

typedef struct {
    const uint8_t *key;
    bool failed, mismatch, checked;
} hw_op_t;

static void hw_encrypt(const void *ctx, const uint8_t in[16], uint8_t out[16]) {
    hw_op_t *op = (hw_op_t *)(uintptr_t)ctx;   /* our own, non-const object */
    if (op->failed || !hw_block(op->key, in, out)) {
        op->failed = true;
        for (unsigned i = 0; i < 16; i++) out[i] = 0;
        return;
    }
    if (!op->checked) {
        op->checked = true;
        uint8_t ref[16];
        aes128_encrypt(&verify_aes, in, ref);
        uint8_t diff = 0;
        for (unsigned i = 0; i < 16; i++) diff |= (uint8_t)(ref[i] ^ out[i]);
        wipe(ref, sizeof ref);
        if (diff) {
            op->mismatch = op->failed = true;
            for (unsigned i = 0; i < 16; i++) out[i] = 0;
        }
    }
}

static bool overlaps(const uint8_t *a, const uint8_t *b, size_t n) {
    return n && (uintptr_t)a < (uintptr_t)b + n && (uintptr_t)b < (uintptr_t)a + n;
}

bool zb_ccm_backend_hw(void *context, bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
                       const uint8_t *aad, size_t aad_len, const uint8_t *input, size_t input_len,
                       uint8_t *output, uint8_t mic[4]) {
    if (!hw_block || hw_stats.hw_disabled || input_len > HW_MAX_INPUT) {
        hw_stats.sw_direct++;
        return zb_ccm_backend_sw(context, encrypt, key, nonce, aad, aad_len, input, input_len, output, mic);
    }
    if (__atomic_exchange_n(&hw_busy, 1u, __ATOMIC_ACQUIRE)) {
        hw_stats.busy++;
        return zb_ccm_backend_sw(context, encrypt, key, nonce, aad, aad_len, input, input_len, output, mic);
    }
    bool same = verify_ready;
    for (unsigned i = 0; i < 16 && same; i++) same = verify_key[i] == key[i];
    if (!same) {
        aes128_init(&verify_aes, key);
        for (unsigned i = 0; i < 16; i++) verify_key[i] = key[i];
        verify_ready = true;
    }
    uint8_t copy[HW_MAX_INPUT];
    const uint8_t *src = input;
    bool copied = output && overlaps(input, output, input_len);
    if (copied) {
        for (size_t i = 0; i < input_len; i++) copy[i] = input[i];
        src = copy;
    }
    hw_op_t op = {key, false, false, false};
    const ccm_block_t bc = {hw_encrypt, &op};
    bool ok = encrypt ? ccm_star_seal_with(&bc, nonce, aad, aad_len, src, input_len, output, mic, 4, true)
                      : ccm_star_open_with(&bc, nonce, aad, aad_len, src, input_len, output, mic, 4, true);
    if (op.failed) {
        hw_stats.hw_faults++;
        if (op.mismatch) {
            hw_stats.hw_mismatches++;
            hw_stats.hw_disabled = true;
        } else if (++hw_consecutive_faults >= HW_FAULT_LIMIT) {
            hw_stats.hw_disabled = true;
        }
        hw_stats.sw_redo++;
        ok = zb_ccm_backend_sw(context, encrypt, key, nonce, aad, aad_len, src, input_len, output, mic);
    } else {
        hw_consecutive_faults = 0;
        hw_stats.hw_ops++;
        bool audit = ++hw_audit_count >= hw_audit_next || (!encrypt && !ok);
        if (audit) {
            if (hw_audit_count >= hw_audit_next) {
                hw_audit_count = 0;
                hw_audit_rng ^= hw_audit_rng << 13; hw_audit_rng ^= hw_audit_rng >> 17; hw_audit_rng ^= hw_audit_rng << 5;
                hw_audit_next = HW_AUDIT_MIN + (hw_audit_rng >> 26);
            }
            hw_stats.hw_audits++;
            uint8_t ref[HW_MAX_INPUT], ref_mic[4];
            for (unsigned i = 0; i < 4; i++) ref_mic[i] = mic[i];
            bool ref_ok = zb_ccm_backend_sw(context, encrypt, key, nonce, aad, aad_len, src, input_len, ref, ref_mic);
            uint8_t diff = (uint8_t)(ref_ok != ok);
            if (ref_ok)
                for (size_t i = 0; i < input_len; i++) diff |= (uint8_t)(ref[i] ^ output[i]);
            if (ref_ok && encrypt)
                for (unsigned i = 0; i < 4; i++) diff |= (uint8_t)(ref_mic[i] ^ mic[i]);
            if (diff) {   /* the software result wins; the accelerator is abandoned */
                hw_stats.hw_audit_failures++;
                hw_stats.hw_disabled = true;
                ok = ref_ok;
                for (size_t i = 0; i < input_len; i++) output[i] = ok ? ref[i] : 0;
                if (encrypt) for (unsigned i = 0; i < 4; i++) mic[i] = ref_mic[i];
            }
            wipe(ref, sizeof ref);
        }
    }
    if (copied) wipe(copy, input_len);
    __atomic_store_n(&hw_busy, 0u, __ATOMIC_RELEASE);
    return ok;
}
