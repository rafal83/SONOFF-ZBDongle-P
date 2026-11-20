/* SPDX-License-Identifier: MIT
 * AES-128 / CCM* / MMO / HMAC-MMO against pyca/cryptography vectors.
 */
#include "frt_test.h"
#include "aes128.h"
#include "ccm_star.h"
#include "zb_mmo.h"
#include "zb_ccm_backend.h"
#include "vectors_crypto.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Simulated hardware AES block: software AES plus injected faults. */
enum { F_NONE, F_ERR_AT, F_ZERO, F_STALE_KEY, F_FLIP, F_REENTER };
static int hw_mode = F_NONE, hw_fail_at = -1, hw_calls, hw_flip_every;
static uint8_t hw_stale_key[16];
static bool reenter_ok;
static bool fake_hw_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    int call = hw_calls++;
    if (hw_mode == F_ERR_AT && call == hw_fail_at) return false;
    if (hw_mode == F_ZERO) { memset(out, 0, 16); return true; }          /* silent */
    aes128_ctx_t a;
    aes128_init(&a, hw_mode == F_STALE_KEY ? hw_stale_key : key);          /* silent */
    aes128_encrypt(&a, in, out);
    aes128_clear(&a);
    if (hw_mode == F_FLIP && call > 0 && hw_flip_every && call % hw_flip_every == 0) out[call % 16] ^= 0x10;
    if (hw_mode == F_REENTER && call == 1) {
        /* Another context needs CCM* while this operation is running. */
        uint8_t k[16] = {1}, n[13] = {2}, p[20] = {3}, c[20], m[4], c2[20], m2[4];
        hw_mode = F_NONE;
        reenter_ok = zb_ccm_backend_hw(0, true, k, n, 0, 0, p, sizeof p, c, m) &&
                     zb_ccm_backend_sw(0, true, k, n, 0, 0, p, sizeof p, c2, m2) &&
                     !memcmp(c, c2, sizeof c) && !memcmp(m, m2, 4);
        hw_mode = F_REENTER;
    }
    return true;
}

typedef struct {
    uint8_t key[16], nonce[13], aad[24], pt[127], ct[127], mic[4];
    size_t alen, plen;
} frame_t;

static void seal_ref(frame_t *f) {
    CHECK(zb_ccm_backend_sw(0, true, f->key, f->nonce, f->aad, f->alen, f->pt, f->plen, f->ct, f->mic));
}

static void random_frame(frame_t *f, size_t min_plen, size_t max_plen) {
    for (unsigned i = 0; i < 16; i++) f->key[i] = (uint8_t)frt_rand();
    for (unsigned i = 0; i < 13; i++) f->nonce[i] = (uint8_t)frt_rand();
    f->alen = frt_rand() % sizeof f->aad;
    f->plen = min_plen + frt_rand() % (max_plen - min_plen + 1);
    for (unsigned i = 0; i < f->alen; i++) f->aad[i] = (uint8_t)frt_rand();
    for (unsigned i = 0; i < f->plen; i++) f->pt[i] = (uint8_t)frt_rand();
    seal_ref(f);
}

static void hw_reset(void) {
    zb_ccm_backend_set_block(fake_hw_block);
    hw_mode = F_NONE; hw_calls = 0; hw_fail_at = -1;
}

static void hw_backend_checks(void) {
    zb_ccm_hw_stats_t st;
    /* 1. Bit-exact parity with the software path, separate and in place,
     *    every payload length 0..127 (the NWK maximum), then random ones. */
    hw_reset();
    for (unsigned round = 0; round < 400; round++) {
        frame_t f;
        if (round < 128) random_frame(&f, round, round);
        else random_frame(&f, 0, 127);
        uint8_t c[127], m[4], buf[127], out[127];
        CHECK(zb_ccm_backend_hw(0, true, f.key, f.nonce, f.aad, f.alen, f.pt, f.plen, c, m));
        CHECK(!memcmp(c, f.ct, f.plen) && !memcmp(m, f.mic, 4));
        memcpy(buf, f.pt, f.plen);
        CHECK(zb_ccm_backend_hw(0, true, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, m));
        CHECK(!memcmp(buf, f.ct, f.plen) && !memcmp(m, f.mic, 4));
        CHECK(zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, f.ct, f.plen, out, f.mic));
        CHECK(!memcmp(out, f.pt, f.plen));
        memcpy(buf, f.ct, f.plen);
        CHECK(zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, f.mic));
        CHECK(!memcmp(buf, f.pt, f.plen));
        uint8_t bad[4] = {(uint8_t)(f.mic[0] ^ 1), f.mic[1], f.mic[2], f.mic[3]};
        memcpy(buf, f.ct, f.plen);
        CHECK(!zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, bad));
        uint8_t zero[127] = {0};
        CHECK(!memcmp(buf, zero, f.plen));   /* no unauthenticated plaintext */
    }
    zb_ccm_backend_hw_stats(&st);
    CHECK(st.hw_faults == 0 && !st.hw_disabled && st.hw_audits > 0 && st.hw_audit_failures == 0);

    /* 2. Reported errors at any block, separate and IN PLACE: the result is
     *    always the software one (sealed frame intact, incoming frame kept,
     *    forged MIC refused), never what the faulty block produced. */
    for (unsigned round = 0; round < 300; round++) {
        hw_reset();
        frame_t f;
        random_frame(&f, 0, 127);
        unsigned blocks = 2u * ((unsigned)(f.plen + 15) / 16u) + (unsigned)(f.alen + 2 + 15) / 16u + 2u;
        uint8_t buf[127], m[4];
        hw_mode = F_ERR_AT;
        hw_calls = 0; hw_fail_at = (int)(frt_rand() % blocks);
        memcpy(buf, f.pt, f.plen);
        CHECK(zb_ccm_backend_hw(0, true, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, m));
        CHECK(!memcmp(buf, f.ct, f.plen) && !memcmp(m, f.mic, 4));
        hw_calls = 0; hw_fail_at = (int)(frt_rand() % blocks);
        memcpy(buf, f.ct, f.plen);
        CHECK(zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, f.mic));
        CHECK(!memcmp(buf, f.pt, f.plen));
        uint8_t bad[4] = {f.mic[0], f.mic[1], f.mic[2], (uint8_t)(f.mic[3] ^ 0x80)};
        hw_calls = 0; hw_fail_at = (int)(frt_rand() % blocks);
        memcpy(buf, f.ct, f.plen);
        CHECK(!zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, bad));
        zb_ccm_backend_hw_stats(&st);
        CHECK(!st.hw_disabled);
    }

    /* 3. Persistent errors: software keeps working, the accelerator is
     *    abandoned after 8 consecutive faulty operations. */
    hw_reset();
    hw_mode = F_ERR_AT;
    for (unsigned round = 0; round < 12; round++) {
        frame_t f;
        random_frame(&f, 0, 60);
        uint8_t c[127], m[4];
        hw_calls = 0; hw_fail_at = 0;
        CHECK(zb_ccm_backend_hw(0, true, f.key, f.nonce, f.aad, f.alen, f.pt, f.plen, c, m));
        CHECK(!memcmp(c, f.ct, f.plen) && !memcmp(m, f.mic, 4));
    }
    zb_ccm_backend_hw_stats(&st);
    CHECK(st.hw_disabled && st.hw_faults == 8 && st.sw_direct >= 4);

    /* 4. Silent all-zero output: with it a MIC of 0 over any payload would
     *    verify. The first-block check refuses the forgery and disables. */
    hw_reset();
    {
        frame_t f;
        random_frame(&f, 1, 60);
        uint8_t forged[127], zero_mic[4] = {0};
        for (size_t i = 0; i < f.plen; i++) forged[i] = (uint8_t)frt_rand();
        hw_mode = F_ZERO;
        CHECK(!zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, forged, f.plen, forged, zero_mic));
        zb_ccm_backend_hw_stats(&st);
        CHECK(st.hw_mismatches == 1 && st.hw_disabled);
        uint8_t buf[127];
        memcpy(buf, f.ct, f.plen);   /* genuine frames still accepted (software) */
        CHECK(zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, f.mic));
        CHECK(!memcmp(buf, f.pt, f.plen));
    }

    /* 5. Silently stale key (e.g. the previous network key after a switch):
     *    a frame sealed by an attacker with the OLD key is refused, and our
     *    own frames are still sealed with the new key. */
    hw_reset();
    {
        frame_t f;
        random_frame(&f, 1, 60);
        for (unsigned i = 0; i < 16; i++) hw_stale_key[i] = (uint8_t)frt_rand();
        uint8_t old_ct[127], old_mic[4];
        CHECK(zb_ccm_backend_sw(0, true, hw_stale_key, f.nonce, f.aad, f.alen, f.pt, f.plen, old_ct, old_mic));
        hw_mode = F_STALE_KEY;
        CHECK(!zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, old_ct, f.plen, old_ct, old_mic));
        uint8_t c[127], m[4];
        CHECK(zb_ccm_backend_hw(0, true, f.key, f.nonce, f.aad, f.alen, f.pt, f.plen, c, m));
        CHECK(!memcmp(c, f.ct, f.plen) && !memcmp(m, f.mic, 4));
        zb_ccm_backend_hw_stats(&st);
        CHECK(st.hw_mismatches == 1 && st.hw_disabled);
    }

    /* 6a. Intermittent silent bit flips after the first block, OPENING: a
     *     frame is never accepted with a wrong plaintext nor forged; the
     *     genuine frame the faulty block made fail is re-verified in software
     *     (accepted, correct) and the accelerator disabled at once. */
    hw_reset();
    hw_mode = F_FLIP;
    hw_flip_every = 3;
    {
        frame_t f;
        random_frame(&f, 40, 100);
        uint8_t buf[127];
        uint8_t bad[4] = {(uint8_t)(f.mic[0] ^ 4), f.mic[1], f.mic[2], f.mic[3]};
        hw_calls = 0;
        memcpy(buf, f.ct, f.plen);
        CHECK(!zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, bad));
        zb_ccm_backend_hw_stats(&st);
        CHECK(!st.hw_disabled);   /* both paths reject a forgery: no evidence yet */
        hw_calls = 0;
        memcpy(buf, f.ct, f.plen);
        CHECK(zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, f.mic));
        CHECK(!memcmp(buf, f.pt, f.plen));
        zb_ccm_backend_hw_stats(&st);
        CHECK(st.hw_disabled && st.hw_audit_failures == 1);
    }
    /* 6b. Same fault while SEALING (silent: the receiver would reject our
     *     frames): caught by the pseudo-random audit within 192 operations,
     *     with any traffic pattern (alternating seal/open here). */
    hw_reset();
    hw_mode = F_FLIP;
    unsigned ops = 0, accepted_wrong = 0, forged_accepted = 0;
    for (; ops < 800; ops += 2) {
        zb_ccm_backend_hw_stats(&st);
        if (st.hw_disabled) break;
        frame_t f;
        random_frame(&f, 40, 100);
        uint8_t buf[127], m[4];
        hw_calls = 0;
        memcpy(buf, f.pt, f.plen);
        (void)zb_ccm_backend_hw(0, true, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, m);
        uint8_t bad[4] = {(uint8_t)(f.mic[0] ^ 4), f.mic[1], f.mic[2], f.mic[3]};
        hw_calls = 0;
        memcpy(buf, f.ct, f.plen);
        if (zb_ccm_backend_hw(0, false, f.key, f.nonce, f.aad, f.alen, buf, f.plen, buf, bad)) forged_accepted++;
        else if (memcmp(buf, (uint8_t[127]){0}, f.plen)) accepted_wrong++;   /* rejected => wiped */
    }
    zb_ccm_backend_hw_stats(&st);
    CHECK(accepted_wrong == 0 && forged_accepted == 0);
    CHECK(st.hw_disabled && st.hw_audit_failures == 1 && ops <= 2 * 192);
    printf("crypto: silent faults while sealing caught by the audit after %u operations\n", ops);

    /* 7. Re-entry during an operation: served by software, both correct. */
    hw_reset();
    {
        frame_t f;
        random_frame(&f, 1, 60);
        uint8_t c[127], m[4];
        hw_mode = F_REENTER; hw_calls = 0; reenter_ok = false;
        CHECK(zb_ccm_backend_hw(0, true, f.key, f.nonce, f.aad, f.alen, f.pt, f.plen, c, m));
        CHECK(reenter_ok && !memcmp(c, f.ct, f.plen) && !memcmp(m, f.mic, 4));
        zb_ccm_backend_hw_stats(&st);
        CHECK(st.busy == 1 && !st.hw_disabled);
    }

    /* 8. Input above 128 bytes (never on the air, but defined): software. */
    hw_reset();
    {
        uint8_t key[16] = {9}, nonce[13] = {8}, big[200], c1[200], c2[200], m1[4], m2[4];
        for (unsigned i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;
        CHECK(zb_ccm_backend_hw(0, true, key, nonce, 0, 0, big, sizeof big, c1, m1));
        CHECK(zb_ccm_backend_sw(0, true, key, nonce, 0, 0, big, sizeof big, c2, m2));
        CHECK(!memcmp(c1, c2, sizeof c1) && !memcmp(m1, m2, 4));
    }
    zb_ccm_backend_set_block(0);
    hw_mode = F_NONE;
}

int main(void) {
    for (size_t i = 0; i < COUNT(AES_VECTORS); i++) {
        aes128_ctx_t c;
        uint8_t out[16];
        aes128_init(&c, AES_VECTORS[i].key);
        aes128_encrypt(&c, AES_VECTORS[i].in, out);
        CHECK(!memcmp(out, AES_VECTORS[i].out, 16));
        aes128_clear(&c);
        for (unsigned k = 0; k < sizeof c.round_key; k++) CHECK(c.round_key[k] == 0);
    }

    for (size_t i = 0; i < COUNT(CCM_VECTORS); i++) {
        const ccm_vec_t *v = &CCM_VECTORS[i];
        aes128_ctx_t c;
        aes128_init(&c, v->key);
        uint8_t ct[80], pt[80], mic[16];
        CHECK(ccm_star_seal(&c, v->nonce, v->aad, v->aad_len, v->pt, v->pt_len, ct, mic, v->m, true));
        CHECK(!memcmp(ct, v->ct, v->pt_len) && !memcmp(mic, v->mic, v->m));
        CHECK(ccm_star_open(&c, v->nonce, v->aad, v->aad_len, v->ct, v->pt_len, pt, v->mic, v->m, true));
        CHECK(!memcmp(pt, v->pt, v->pt_len));
        /* Any single-bit change in MIC, ciphertext or AAD is rejected and the
         * output is wiped. */
        for (size_t bit = 0; bit < 8 * v->m; bit++) {
            memcpy(mic, v->mic, v->m);
            mic[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            memset(pt, 0xA5, sizeof pt);
            CHECK(!ccm_star_open(&c, v->nonce, v->aad, v->aad_len, v->ct, v->pt_len, pt, mic, v->m, true));
            for (size_t k = 0; k < v->pt_len; k++) CHECK(pt[k] == 0);
        }
        for (size_t bit = 0; bit < 8 * v->pt_len; bit += 7) {
            memcpy(ct, v->ct, v->pt_len);
            ct[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            CHECK(!ccm_star_open(&c, v->nonce, v->aad, v->aad_len, ct, v->pt_len, pt, v->mic, v->m, true));
        }
        if (v->aad_len) {
            uint8_t aad[40];
            memcpy(aad, v->aad, v->aad_len);
            aad[v->aad_len - 1] ^= 1;
            CHECK(!ccm_star_open(&c, v->nonce, aad, v->aad_len, v->ct, v->pt_len, pt, v->mic, v->m, true));
        }
    }
    /* In-place encryption and decryption. */
    {
        const ccm_vec_t *v = &CCM_VECTORS[0];
        aes128_ctx_t c;
        aes128_init(&c, v->key);
        uint8_t buf[80], mic[16];
        memcpy(buf, v->pt, v->pt_len);
        CHECK(ccm_star_seal(&c, v->nonce, v->aad, v->aad_len, buf, v->pt_len, buf, mic, v->m, true));
        CHECK(!memcmp(buf, v->ct, v->pt_len));
        CHECK(ccm_star_open(&c, v->nonce, v->aad, v->aad_len, buf, v->pt_len, buf, mic, v->m, true));
        CHECK(!memcmp(buf, v->pt, v->pt_len));
        /* Parameter validation. */
        CHECK(!ccm_star_seal(&c, v->nonce, 0, 0, buf, 1, buf, mic, 6, true));
        CHECK(!ccm_star_seal(&c, v->nonce, 0, 0, buf, 1, buf, mic, 0, false));
        CHECK(ccm_star_seal(&c, v->nonce, 0, 0, buf, 1, buf, 0, 0, true));   /* ENC only (level 4) */
        CHECK(ccm_star_seal(&c, v->nonce, v->aad, v->aad_len, buf, 4, buf, mic, 4, false)); /* MIC only */
        CHECK(ccm_star_open(&c, v->nonce, v->aad, v->aad_len, buf, 4, buf, mic, 4, false));
    }

    for (size_t i = 0; i < COUNT(MMO_VECTORS); i++) {
        uint8_t d[16];
        CHECK(zb_mmo_hash(MMO_VECTORS[i].msg, MMO_VECTORS[i].len, d));
        CHECK(!memcmp(d, MMO_VECTORS[i].digest, 16));
    }
    for (size_t i = 0; i < COUNT(HMAC_VECTORS); i++) {
        uint8_t m[16];
        CHECK(zb_hmac_mmo(HMAC_VECTORS[i].key, HMAC_VECTORS[i].msg, HMAC_VECTORS[i].len, m));
        CHECK(!memcmp(m, HMAC_VECTORS[i].mac, 16));
    }
    static uint8_t big[8192];
    uint8_t d[16];
    CHECK(!zb_mmo_hash(big, sizeof big, d));
    CHECK(zb_mmo_hash(big, sizeof big - 1, d));

    /* Published install-code example: code 83FED340 7A939723 A5C639B2
     * 6916D505 + CRC C3B5 -> link key 66B69009 81E1EE3C A4206B6B 861C02BB. */
    const uint8_t code[18] = {0x83, 0xFE, 0xD3, 0x40, 0x7A, 0x93, 0x97, 0x23, 0xA5, 0xC6,
                              0x39, 0xB2, 0x69, 0x16, 0xD5, 0x05, 0xC3, 0xB5};
    const uint8_t key[16] = {0x66, 0xB6, 0x90, 0x09, 0x81, 0xE1, 0xEE, 0x3C,
                             0xA4, 0x20, 0x6B, 0x6B, 0x86, 0x1C, 0x02, 0xBB};
    uint8_t out[16];
    CHECK(zb_install_code_crc(code, 16) == 0xB5C3);
    CHECK(zb_install_code_to_key(code, 18, out) && !memcmp(out, key, 16));
    uint8_t broken[18];
    memcpy(broken, code, 18);
    broken[16] ^= 1;
    CHECK(!zb_install_code_to_key(broken, 18, out));
    CHECK(!zb_install_code_to_key(code, 17, out));

    /* Derived keys are HMAC-MMO of a single byte and differ from each other. */
    uint8_t tk[16], lk[16], ref[16];
    zb_key_transport_key(key, tk);
    zb_key_load_key(key, lk);
    const uint8_t zero = 0x00;
    CHECK(zb_hmac_mmo(key, &zero, 1, ref) && !memcmp(tk, ref, 16));
    CHECK(memcmp(tk, lk, 16) != 0);
    hw_backend_checks();
    PASS("crypto");
    return 0;
}
