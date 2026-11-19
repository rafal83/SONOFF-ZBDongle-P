/* SPDX-License-Identifier: MIT
 * CC2652P AES-128 accelerator (ECB, one block). See hal_aes.c for status.
 */
#ifndef HAL_AES_H
#define HAL_AES_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t blocks, key_loads, key_errors, dma_errors, timeouts;
    uint32_t resets, reset_timeouts, busy;
} hal_aes_stats_t;

/* Encrypt one block; false on any accelerator error or if another operation
 * is running (the caller falls back to software). Never blocks unbounded. */
bool hal_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* Forget the loaded key (needed after any power-down of the crypto domain). */
void hal_aes_invalidate(void);
/* FIPS-197 vector + comparison with the software AES on random blocks. */
bool hal_aes_selftest(void);
void hal_aes_get_stats(hal_aes_stats_t *out);
#endif
