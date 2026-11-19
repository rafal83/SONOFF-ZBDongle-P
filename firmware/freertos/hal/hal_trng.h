/* SPDX-License-Identifier: MIT
 * CC26x2 hardware TRNG (polled, bounded). Raw entropy; keys are derived by
 * conditioning several TRNG words through the Zigbee MMO hash.
 */
#ifndef FRT_HAL_TRNG_H
#define FRT_HAL_TRNG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* false if the TRNG produced no number within its time budget. */
bool hal_trng_read(uint8_t *out, size_t len);
/* 16-byte key = MMO(32 bytes of TRNG output). */
bool hal_trng_key(uint8_t key[16]);
uint32_t hal_trng_u32(void);
#endif
