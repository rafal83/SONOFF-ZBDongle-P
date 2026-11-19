/* SPDX-License-Identifier: MIT
 * CC2652P AES-128 accelerator: ECB (one block) and native AES-CCM* (L = 2,
 * M = 4, one pass of the engine). See hal_aes.c for status.
 */
#ifndef HAL_AES_H
#define HAL_AES_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t blocks, key_loads, key_errors, dma_errors, timeouts;
    uint32_t resets, reset_timeouts, busy;
    uint32_t ccm_ops;              /* native CCM* operations completed */
    uint32_t key_check_failures;   /* engine key differed from the requested one */
} hal_aes_stats_t;

#define HAL_AES_CCM_MAX 128u   /* AAD and data, bytes (one Zigbee frame) */

/* Encrypt one block; false on any accelerator error or if another operation
 * is running (the caller falls back to software). Never blocks unbounded. */
bool hal_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* Native AES-CCM* (Zigbee: nonce 13 bytes, MIC 4 bytes), 1..HAL_AES_CCM_MAX
 * bytes of AAD and of data. Encrypt: `out` = ciphertext, `tag` = MIC.
 * Decrypt: `out` = plaintext, `tag` = the MIC computed by the engine (the
 * CALLER compares it with the received one and discards `out` if they
 * differ). Before each operation the engine's key is checked: a known block
 * is encrypted by the engine and compared with the software result computed
 * when that key was loaded. false on any error, refused length or busy. */
bool hal_aes_ccm(const uint8_t key[16], bool encrypt, const uint8_t nonce[13], const uint8_t *aad, unsigned aad_len,
                 const uint8_t *in, unsigned len, uint8_t *out, uint8_t tag[4]);
/* Native CCM* against the software CCM* (seal and open, odd lengths). */
bool hal_aes_ccm_selftest(void);
/* Forget the loaded key (needed after any power-down of the crypto domain). */
void hal_aes_invalidate(void);
/* FIPS-197 vector + comparison with the software AES on random blocks. */
bool hal_aes_selftest(void);
void hal_aes_get_stats(hal_aes_stats_t *out);
#endif
