/* SPDX-License-Identifier: MIT
 * Independent NWK ENC-MIC-32 frame path with indexed peer replay state.
 * This is NOT an APS key-transport, Trust Center or Green Power engine.
 * No software AES: the adapter supplies authenticated AES-CCM and durable NV.
 */
#ifndef CORE_SECURITY_H
#define CORE_SECURITY_H
#include "core_index.h"
#include <stddef.h>
#define CORE_SECURITY_PEERS 200
#define CORE_SECURITY_MAX_FRAME 127
#define CORE_SECURITY_RESERVATION 1001u
typedef bool (*core_security_ccm_t)(void *context, bool encrypt,
    const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
    const uint8_t *input, size_t input_len, uint8_t *output, uint8_t mic[4]);
/* Persist exclusive upper bound atomically; return true only after durability. */
typedef bool (*core_security_reserve_t)(void *context, uint32_t upper_bound);
/* Epoch zero is invalid. Refuse 16-bit epoch exhaustion rather than wrapping
 * and accidentally reviving a stale replay context. */
typedef struct { uint32_t counter[2]; uint16_t epoch[2]; } core_security_peer_t;
typedef struct { uint8_t key[16], sequence; bool valid; uint16_t epoch; } core_security_key_t;
typedef struct {
    core_index_t peers;
    core_index_branch_t branches[CORE_SECURITY_PEERS-1];
    uint64_t addresses[CORE_SECURITY_PEERS];
    uint8_t occupied[(CORE_SECURITY_PEERS+7)/8];
    core_security_peer_t replay[CORE_SECURITY_PEERS];
    uint16_t free_next[CORE_SECURITY_PEERS], free_head;
    core_security_key_t network_keys[2];
    uint32_t tx_next, tx_reserved;
    core_security_ccm_t ccm;
    core_security_reserve_t reserve;
    void *context;
} core_security_t;
void core_security_init(core_security_t *, uint32_t persisted_exclusive_upper,
    core_security_ccm_t, core_security_reserve_t, void *);
void core_security_clear(core_security_t *);
bool core_security_set_key(core_security_t *, unsigned slot, uint8_t sequence, const uint8_t key[16]);
bool core_security_register(core_security_t *, const uint8_t address[8]);
bool core_security_unregister(core_security_t *, const uint8_t address[8]);
/* Last authenticated incoming counter of `address` under the key `sequence`
 * (false: none for that key). */
bool core_security_peer_counter(core_security_t *, const uint8_t address[8], uint8_t sequence, uint32_t *counter);
bool core_security_restore_peer_counter(core_security_t *, const uint8_t address[8],
    uint8_t sequence, uint32_t authenticated_counter);
bool core_security_seal(core_security_t *, unsigned key_slot, const uint8_t local_address[8],
    uint8_t *frame, size_t header_len, size_t payload_len, size_t capacity, size_t *frame_len);
bool core_security_open(core_security_t *, uint8_t *frame, size_t frame_len,
    size_t header_len, size_t *payload_len);
/* Populate a replay-cache entry only AFTER successful authentication. This
 * grants no Trust Center or membership permission; that policy remains above. */
bool core_security_open_authenticated_peer(core_security_t *, uint8_t *, size_t, size_t, size_t *);
#endif
