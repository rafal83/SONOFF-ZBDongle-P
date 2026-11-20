/* SPDX-License-Identifier: MIT
 * Software AES-CCM* backend matching core_security_ccm_t (M = 4, L = 2).
 * Lets the existing NWK security engine (firmware/src/core_security.c)
 * run without TI's crypto driver. A hardware backend with the same
 * signature can replace it on the MCU.
 */
#ifndef ZB_CCM_BACKEND_H
#define ZB_CCM_BACKEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_security.h"

bool zb_ccm_backend_sw(void *context, bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
                       const uint8_t *aad, size_t aad_len, const uint8_t *input, size_t input_len,
                       uint8_t *output, uint8_t mic[4]);
/* Same CCM*, AES blocks from the hardware hook (zb_ccm_backend_set_block).
 * Any failed block, or a first block that differs from the software AES,
 * makes the whole operation redo in software from an intact copy of the
 * input (in-place calls included). See zb_ccm_backend.c for the rules. */
bool zb_ccm_backend_hw(void *context, bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
                       const uint8_t *aad, size_t aad_len, const uint8_t *input, size_t input_len,
                       uint8_t *output, uint8_t mic[4]);
typedef bool (*zb_aes_block_fn)(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void zb_ccm_backend_set_block(zb_aes_block_fn fn);
/* Native CCM* of the accelerator (hal_aes_ccm): preferred over the block
 * hook when installed, for 1..128 bytes of AAD and of data. Decrypt returns
 * the MIC computed by the engine in `tag`; the backend compares it. */
typedef bool (*zb_ccm_native_fn)(const uint8_t key[16], bool encrypt, const uint8_t nonce[13], const uint8_t *aad,
                                 unsigned aad_len, const uint8_t *in, unsigned len, uint8_t *out, uint8_t tag[4]);
void zb_ccm_backend_set_native(zb_ccm_native_fn fn);
typedef struct {
    uint32_t hw_ops;         /* operations completed by the accelerator */
    uint32_t hw_faults;      /* operations with a failed or wrong block */
    uint32_t hw_mismatches;  /* first block differed from the software AES */
    uint32_t sw_redo;        /* operations redone in software after a fault */
    uint32_t sw_direct;      /* software only (no/disabled hook, input > 128) */
    uint32_t busy;           /* concurrent call, served by software */
    uint32_t hw_audits;      /* recomputed in software (1 in 32..95 + every rejection) */
    uint32_t hw_audit_failures;
    bool hw_disabled;        /* accelerator abandoned until reboot */
    uint32_t native_ops;     /* of hw_ops: done by the native CCM* of the engine */
    uint32_t tag_suspect;    /* native MIC all zero, or repeated for another nonce: redone */
    uint32_t reject_unverified;   /* rejections not re-verified in software (budget used up) */
} zb_ccm_hw_stats_t;
void zb_ccm_backend_hw_stats(zb_ccm_hw_stats_t *out);
/* Backend used by the coordinator (software unless the platform selected
 * the hardware one after its self-test). */
extern core_security_ccm_t zb_ccm_backend_active;
#endif
