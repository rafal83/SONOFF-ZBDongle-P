/* SPDX-License-Identifier: MIT
 * CCM* (IEEE 802.15.4 Annex B / Zigbee Annex A) with a 13-byte nonce (L=2).
 * MIC length M in {0, 4, 8, 16}; M = 0 is encryption without
 * authentication (security levels 4) and is only allowed with encryption.
 * Decryption verifies the MIC in constant time and wipes the output on
 * failure, so no unauthenticated plaintext ever leaves this module.
 */
#ifndef ZB_CCM_STAR_H
#define ZB_CCM_STAR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aes128.h"

#define CCM_STAR_NONCE_LEN 13u
#define CCM_STAR_MAX_AAD 0xFEFFu
#define CCM_STAR_MAX_DATA 0xFFFFu

/* Any AES-128 block encryption (software round keys, or the CC2652P
 * accelerator): CCM* only ever encrypts blocks. */
typedef struct {
    void (*encrypt)(const void *ctx, const uint8_t in[16], uint8_t out[16]);
    const void *ctx;
} ccm_block_t;
bool ccm_star_seal_with(const ccm_block_t *block, const uint8_t nonce[13],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t len, uint8_t *out,
                        uint8_t *mic, size_t mic_len, bool encrypt);
bool ccm_star_open_with(const ccm_block_t *block, const uint8_t nonce[13],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *in, size_t len, uint8_t *out,
                        const uint8_t *mic, size_t mic_len, bool encrypted);

/* Software AES. encrypt=false: authentication only (output = input copy). */
bool ccm_star_seal(const aes128_ctx_t *aes, const uint8_t nonce[13],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *in, size_t len, uint8_t *out,
                   uint8_t *mic, size_t mic_len, bool encrypt);
bool ccm_star_open(const aes128_ctx_t *aes, const uint8_t nonce[13],
                   const uint8_t *aad, size_t aad_len,
                   const uint8_t *in, size_t len, uint8_t *out,
                   const uint8_t *mic, size_t mic_len, bool encrypted);
#endif
