/* SPDX-License-Identifier: MIT
 * CCM* written from RFC 3610 / IEEE 802.15.4-2006 Annex B.
 */
#include "ccm_star.h"

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

static bool valid_m(size_t m) { return m == 0 || m == 4 || m == 8 || m == 16; }

/* CBC-MAC over B0 | encoded AAD | message, returns T (16 bytes). */
static void cbc_mac(const ccm_block_t *aes, const uint8_t nonce[13], const uint8_t *aad,
                    size_t aad_len, const uint8_t *msg, size_t len, size_t m, uint8_t tag[16]) {
    uint8_t x[16];
    uint8_t b0[16];
    b0[0] = (uint8_t)((aad_len ? 0x40u : 0u) | (((m - 2u) / 2u) << 3) | 1u); /* L-1 = 1 */
    for (unsigned i = 0; i < 13; i++) b0[1 + i] = nonce[i];
    b0[14] = (uint8_t)(len >> 8);
    b0[15] = (uint8_t)len;
    aes->encrypt(aes->ctx, b0, x);
    if (aad_len) {
        uint8_t block[16];
        size_t used = 2, i = 0;
        block[0] = (uint8_t)(aad_len >> 8);
        block[1] = (uint8_t)aad_len;
        while (i < aad_len || used) {
            while (used < 16 && i < aad_len) block[used++] = aad[i++];
            while (used < 16) block[used++] = 0;
            for (unsigned k = 0; k < 16; k++) x[k] ^= block[k];
            aes->encrypt(aes->ctx, x, x);
            used = 0;
        }
    }
    for (size_t i = 0; i < len; i += 16) {
        size_t n = len - i < 16 ? len - i : 16;
        for (size_t k = 0; k < n; k++) x[k] ^= msg[i + k];
        aes->encrypt(aes->ctx, x, x);
    }
    for (unsigned k = 0; k < 16; k++) tag[k] = x[k];
    wipe(x, sizeof x);
}

static void ctr_block(const ccm_block_t *aes, const uint8_t nonce[13], uint16_t counter, uint8_t s[16]) {
    uint8_t a[16];
    a[0] = 1u; /* L-1 */
    for (unsigned i = 0; i < 13; i++) a[1 + i] = nonce[i];
    a[14] = (uint8_t)(counter >> 8);
    a[15] = (uint8_t)counter;
    aes->encrypt(aes->ctx, a, s);
}

static void ctr_crypt(const ccm_block_t *aes, const uint8_t nonce[13], const uint8_t *in,
                      size_t len, uint8_t *out) {
    uint8_t s[16];
    uint16_t counter = 1;
    for (size_t i = 0; i < len; i += 16, counter++) {
        ctr_block(aes, nonce, counter, s);
        size_t n = len - i < 16 ? len - i : 16;
        for (size_t k = 0; k < n; k++) out[i + k] = in[i + k] ^ s[k];
    }
    wipe(s, sizeof s);
}

bool ccm_star_seal_with(const ccm_block_t *aes, const uint8_t nonce[13], const uint8_t *aad,
                   size_t aad_len, const uint8_t *in, size_t len, uint8_t *out,
                   uint8_t *mic, size_t m, bool encrypt) {
    if (!valid_m(m) || (m == 0 && !encrypt) || aad_len > CCM_STAR_MAX_AAD || len > CCM_STAR_MAX_DATA ||
        (aad_len && !aad) || (len && (!in || !out)) || (m && !mic)) return false;
    if (m) {
        uint8_t tag[16], s0[16];
        cbc_mac(aes, nonce, aad, aad_len, in, len, m, tag);
        ctr_block(aes, nonce, 0, s0);
        for (size_t k = 0; k < m; k++) mic[k] = tag[k] ^ s0[k];
        wipe(tag, sizeof tag);
        wipe(s0, sizeof s0);
    }
    if (encrypt) ctr_crypt(aes, nonce, in, len, out);
    else if (out != in) for (size_t i = 0; i < len; i++) out[i] = in[i];
    return true;
}

bool ccm_star_open_with(const ccm_block_t *aes, const uint8_t nonce[13], const uint8_t *aad,
                   size_t aad_len, const uint8_t *in, size_t len, uint8_t *out,
                   const uint8_t *mic, size_t m, bool encrypted) {
    if (!valid_m(m) || (m == 0 && !encrypted) || aad_len > CCM_STAR_MAX_AAD || len > CCM_STAR_MAX_DATA ||
        (aad_len && !aad) || (len && (!in || !out)) || (m && !mic)) return false;
    if (encrypted) ctr_crypt(aes, nonce, in, len, out);
    else if (out != in) for (size_t i = 0; i < len; i++) out[i] = in[i];
    if (!m) return true;
    uint8_t tag[16], s0[16];
    cbc_mac(aes, nonce, aad, aad_len, out, len, m, tag);
    ctr_block(aes, nonce, 0, s0);
    uint8_t diff = 0;
    for (size_t k = 0; k < m; k++) diff |= (uint8_t)(tag[k] ^ s0[k] ^ mic[k]);
    wipe(tag, sizeof tag);
    wipe(s0, sizeof s0);
    if (diff) {
        wipe(out, len);
        return false;
    }
    return true;
}

static void aes_sw_block(const void *ctx, const uint8_t in[16], uint8_t out[16]) {
    aes128_encrypt((const aes128_ctx_t *)ctx, in, out);
}

bool ccm_star_seal(const aes128_ctx_t *aes, const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                   const uint8_t *in, size_t len, uint8_t *out, uint8_t *mic, size_t m, bool encrypt) {
    const ccm_block_t bc = {aes_sw_block, aes};
    return ccm_star_seal_with(&bc, nonce, aad, aad_len, in, len, out, mic, m, encrypt);
}

bool ccm_star_open(const aes128_ctx_t *aes, const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                   const uint8_t *in, size_t len, uint8_t *out, const uint8_t *mic, size_t m, bool encrypted) {
    const ccm_block_t bc = {aes_sw_block, aes};
    return ccm_star_open_with(&bc, nonce, aad, aad_len, in, len, out, mic, m, encrypted);
}
