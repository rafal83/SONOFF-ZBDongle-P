/* SPDX-License-Identifier: MIT */
#include "zb_mmo.h"
#include "aes128.h"

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

static void mmo_block(uint8_t hash[16], const uint8_t block[16]) {
    aes128_ctx_t ctx;
    uint8_t out[16];
    aes128_init(&ctx, hash);
    aes128_encrypt(&ctx, block, out);
    for (unsigned i = 0; i < 16; i++) hash[i] = out[i] ^ block[i];
    aes128_clear(&ctx);
    wipe(out, sizeof out);
}

/* Streaming MMO over up to two fragments (used by HMAC). */
static bool mmo2(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t digest[16]) {
    size_t total = alen + blen;
    if (total >= 8192u) return false;
    uint8_t hash[16] = {0}, block[16];
    size_t used = 0;
    for (size_t i = 0; i < total; i++) {
        block[used++] = i < alen ? a[i] : b[i - alen];
        if (used == 16) { mmo_block(hash, block); used = 0; }
    }
    /* Padding: 1 bit, zeros, then the 16-bit bit length, to a block boundary. */
    block[used++] = 0x80;
    if (used > 14) {
        while (used < 16) block[used++] = 0;
        mmo_block(hash, block);
        used = 0;
    }
    while (used < 14) block[used++] = 0;
    uint16_t bits = (uint16_t)(total * 8u);
    block[14] = (uint8_t)(bits >> 8);
    block[15] = (uint8_t)bits;
    mmo_block(hash, block);
    for (unsigned i = 0; i < 16; i++) digest[i] = hash[i];
    wipe(hash, sizeof hash);
    wipe(block, sizeof block);
    return true;
}

bool zb_mmo_hash(const uint8_t *msg, size_t len, uint8_t digest[16]) {
    return mmo2(msg, len, 0, 0, digest);
}

bool zb_hmac_mmo(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t mac[16]) {
    /* Block size 16 = key size: the key is used as is (FIPS 198). */
    uint8_t pad[16], inner[16];
    for (unsigned i = 0; i < 16; i++) pad[i] = key[i] ^ 0x36u;
    if (!mmo2(pad, 16, msg, len, inner)) { wipe(pad, sizeof pad); return false; }
    for (unsigned i = 0; i < 16; i++) pad[i] = key[i] ^ 0x5Cu;
    bool ok = mmo2(pad, 16, inner, 16, mac);
    wipe(pad, sizeof pad);
    wipe(inner, sizeof inner);
    return ok;
}

void zb_key_transport_key(const uint8_t link_key[16], uint8_t out[16]) {
    const uint8_t input = 0x00;
    (void)zb_hmac_mmo(link_key, &input, 1, out);
}

void zb_key_load_key(const uint8_t link_key[16], uint8_t out[16]) {
    const uint8_t input = 0x02;
    (void)zb_hmac_mmo(link_key, &input, 1, out);
}

uint16_t zb_install_code_crc(const uint8_t *code, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= code[i];
        for (unsigned b = 0; b < 8; b++) crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)~crc;
}

bool zb_install_code_to_key(const uint8_t *code, size_t len, uint8_t key[16]) {
    if (len != 8 && len != 10 && len != 14 && len != 18) return false;
    uint16_t crc = zb_install_code_crc(code, len - 2);
    if (code[len - 2] != (uint8_t)crc || code[len - 1] != (uint8_t)(crc >> 8)) return false;
    return zb_mmo_hash(code, len, key);
}
