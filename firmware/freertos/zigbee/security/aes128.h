/* SPDX-License-Identifier: MIT
 * Portable AES-128 block encryption (FIPS-197). Encryption only: CCM*, MMO
 * and HMAC-MMO never need the inverse cipher. It uses a 256-byte S-box
 * lookup, which is NOT constant-time on CPUs with data caches; the CC2652P
 * has no data cache on SRAM/flash reads of this kind, but this property is
 * stated, not measured.
 * The MCU build is expected to use the CC2652P AES accelerator for bulk work;
 * this implementation is the reference and fallback.
 */
#ifndef ZB_AES128_H
#define ZB_AES128_H
#include <stdint.h>

typedef struct { uint8_t round_key[176]; } aes128_ctx_t;

void aes128_init(aes128_ctx_t *ctx, const uint8_t key[16]);
void aes128_encrypt(const aes128_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);
void aes128_clear(aes128_ctx_t *ctx);
#endif
