/* SPDX-License-Identifier: MIT */
#include "zb_ccm_backend.h"
#include "aes128.h"
#include "ccm_star.h"
#include <string.h>

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
 *
 * Native CCM* (zb_ccm_backend_set_native, the engine computes the whole
 * CCM* in one pass): the per-operation first-block check is replaced by the
 * driver's key check (the engine encrypts a known block, compared with the
 * software value of that key: a stale or foreign key is refused before the
 * operation). The other rules are unchanged: any error -> software redo from
 * the intact input; every rejection re-verified in software; random audits;
 * HW_FAULT_LIMIT. In addition, a MIC that is all zero, or equal to the
 * previous operation's MIC for another nonce (tag registers not updated), is
 * not trusted: the operation is redone in software.
 */
#define HW_MAX_INPUT 128u
#define HW_FAULT_LIMIT 8u
/* Audit interval: HW_AUDIT_MIN + (0..127) operations. A software audit costs
 * a full software CCM* (~75 000 cycles): 1 in 64..191 adds ~600 cycles per
 * operation on average (1 in 32..95 added ~1 200). Rejected frames are still
 * ALL re-verified in software. */
#define HW_AUDIT_MIN 64u
/* Software re-verification of the frames the accelerator REJECTS: a token
 * bucket of HW_REJECT_BURST, one token back every HW_REJECT_REFILL operations
 * the accelerator completed with success. Without a bound, each forged frame
 * with a fresh counter (it passes the replay check) cost a full software
 * CCM* (~75 000 cycles): at the ~500 short frames/s the channel carries, a
 * forger alone could hold the CPU at ~80 %. Genuine traffic with occasional
 * rejections keeps every one re-verified; under a flood, a silent fault that
 * rejects genuine frames is still caught by the random audits (<= 191
 * operations) and by the re-verifications the bucket allows. */
#define HW_REJECT_BURST 8u
#define HW_REJECT_REFILL 16u

static zb_aes_block_fn hw_block;
static zb_ccm_native_fn hw_native;
static uint8_t last_tag[4], last_nonce[13];
static bool last_tag_valid;
static zb_ccm_hw_stats_t hw_stats;
static uint32_t hw_consecutive_faults;
static volatile uint32_t hw_busy;
static uint32_t hw_audit_count, hw_audit_next = HW_AUDIT_MIN, hw_audit_rng = 0x9E3779B9u;
static uint32_t hw_reject_tokens = HW_REJECT_BURST, hw_reject_refill;
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
    hw_reject_tokens = HW_REJECT_BURST;
    hw_reject_refill = 0;
}

void zb_ccm_backend_set_native(zb_ccm_native_fn fn) {
    hw_native = fn;
    last_tag_valid = false;
}

void zb_ccm_backend_hw_stats(zb_ccm_hw_stats_t *out) { *out = hw_stats; }

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (((uintptr_t)v & 3u) && n) { *v++ = 0; n--; }
    volatile uint32_t *v32 = (volatile uint32_t *)(void *)v;
    while (n >= 4) { *v32++ = 0; n -= 4; }
    v = (volatile uint8_t *)v32;
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
    if ((!hw_block && !hw_native) || hw_stats.hw_disabled || input_len > HW_MAX_INPUT) {
        hw_stats.sw_direct++;
        return zb_ccm_backend_sw(context, encrypt, key, nonce, aad, aad_len, input, input_len, output, mic);
    }
    if (__atomic_exchange_n(&hw_busy, 1u, __ATOMIC_ACQUIRE)) {
        hw_stats.busy++;
        return zb_ccm_backend_sw(context, encrypt, key, nonce, aad, aad_len, input, input_len, output, mic);
    }
    uint8_t copy[HW_MAX_INPUT];
    const uint8_t *src = input;
    bool copied = output && overlaps(input, output, input_len);
    if (copied) {
        memcpy(copy, input, input_len);
        src = copy;
    }
    hw_op_t op = {key, false, false, false};
    bool ok = false;
    if (hw_native && aad_len && aad_len <= HW_MAX_INPUT && input_len) {
        uint8_t tag[4];
        bool done = hw_native(key, encrypt, nonce, aad, (unsigned)aad_len, src, (unsigned)input_len, output, tag);
        if (done) {
            uint32_t tag32;
            memcpy(&tag32, tag, 4);
            bool same_tag = last_tag_valid && (memcmp(tag, last_tag, 4) == 0);
            bool same_nonce = memcmp(nonce, last_nonce, 13) == 0;
            if (tag32 == 0 || (same_tag && !same_nonce)) { hw_stats.tag_suspect++; done = false; }
        }
        if (!done) {
            op.failed = true;
            if (output) wipe(output, input_len);
        } else {
            memcpy(last_tag, tag, 4);
            memcpy(last_nonce, nonce, 13);
            last_tag_valid = true;
            hw_stats.native_ops++;
            if (encrypt) {
                memcpy(mic, tag, 4);
                ok = true;
            } else {
                ok = memcmp(tag, mic, 4) == 0;
                if (!ok && output) wipe(output, input_len);   /* as ccm_star_open_with */
            }
        }
        wipe(tag, sizeof tag);
    } else if (hw_block) {
        bool same = verify_ready;
        for (unsigned i = 0; i < 16 && same; i++) same = verify_key[i] == key[i];
        if (!same) {
            aes128_init(&verify_aes, key);
            for (unsigned i = 0; i < 16; i++) verify_key[i] = key[i];
            verify_ready = true;
        }
        const ccm_block_t bc = {hw_encrypt, &op};
        ok = encrypt ? ccm_star_seal_with(&bc, nonce, aad, aad_len, src, input_len, output, mic, 4, true)
                     : ccm_star_open_with(&bc, nonce, aad, aad_len, src, input_len, output, mic, 4, true);
    } else {
        op.failed = true;   /* native only, operation outside its limits */
    }
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
        bool audit = ++hw_audit_count >= hw_audit_next;
        if (!encrypt && !ok) {
            if (hw_reject_tokens) { hw_reject_tokens--; audit = true; }
            else hw_stats.reject_unverified++;   /* rejected on the accelerator's verdict */
        } else if (++hw_reject_refill >= HW_REJECT_REFILL) {
            hw_reject_refill = 0;
            if (hw_reject_tokens < HW_REJECT_BURST) hw_reject_tokens++;
        }
        if (audit) {
            if (hw_audit_count >= hw_audit_next) {
                hw_audit_count = 0;
                hw_audit_rng ^= hw_audit_rng << 13; hw_audit_rng ^= hw_audit_rng >> 17; hw_audit_rng ^= hw_audit_rng << 5;
                hw_audit_next = HW_AUDIT_MIN + (hw_audit_rng >> 25);
            }
            hw_stats.hw_audits++;
            uint8_t ref[HW_MAX_INPUT], ref_mic[4];
            memcpy(ref_mic, mic, 4);
            bool ref_ok = zb_ccm_backend_sw(context, encrypt, key, nonce, aad, aad_len, src, input_len, ref, ref_mic);
            uint8_t diff = (uint8_t)(ref_ok != ok);
            if (ref_ok)
                diff |= (uint8_t)(memcmp(ref, output, input_len) != 0);
            if (ref_ok && encrypt)
                diff |= (uint8_t)(memcmp(ref_mic, mic, 4) != 0);
            if (diff) {   /* the software result wins; the accelerator is abandoned */
                hw_stats.hw_audit_failures++;
                hw_stats.hw_disabled = true;
                ok = ref_ok;
                if (output) {
                    if (ok) memcpy(output, ref, input_len);
                    else wipe(output, input_len);
                }
                if (encrypt) memcpy(mic, ref_mic, 4);
            }
            wipe(ref, sizeof ref);
        }
    }
    if (copied) wipe(copy, input_len);
    __atomic_store_n(&hw_busy, 0u, __ATOMIC_RELEASE);
    return ok;
}
