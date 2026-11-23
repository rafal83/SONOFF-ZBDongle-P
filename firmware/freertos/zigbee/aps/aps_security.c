/* SPDX-License-Identifier: MIT */
#include "aps_security.h"
#include "aes128.h"
#include "ccm_star.h"

#define APS_MAX_FRAME 127u

static uint8_t real_sc(const nwk_aux_t *a) {
    return (uint8_t)(APS_SECURITY_LEVEL | (a->key_id << 3) | (a->extended_nonce << 5));
}

size_t aps_seal(const uint8_t key[16], const uint8_t *hdr, size_t hdr_len, const nwk_aux_t *aux,
                const uint8_t *payload, size_t len, uint8_t *out, size_t cap) {
    if (!key || !hdr || !aux || !out || hdr_len < 2 || hdr_len > APS_MAX_FRAME || aux->key_id > 3 ||
        (len && !payload)) return 0;
    /* Without the extended nonce, aux->source is still the sender's IEEE (in
     * the nonce), just not transmitted. */
    nwk_aux_t on_air = *aux;
    on_air.level = 0;
    uint8_t aux_bytes[14];
    size_t aux_len = nwk_aux_build(&on_air, aux_bytes, sizeof aux_bytes);
    size_t total = hdr_len + aux_len + len + 4u;
    if (!aux_len || total > cap || total > APS_MAX_FRAME) return 0;
    uint8_t aad[APS_MAX_FRAME];
    for (size_t i = 0; i < hdr_len; i++) aad[i] = hdr[i];
    aad[0] |= 0x20u;
    for (size_t i = 0; i < aux_len; i++) aad[hdr_len + i] = aux_bytes[i];
    aad[hdr_len] = real_sc(aux);
    uint8_t nonce[13];
    nwk_nonce(aux->source, aux->frame_counter, real_sc(aux), nonce);
    aes128_ctx_t aes;
    aes128_init(&aes, key);
    uint8_t *cipher = out + hdr_len + aux_len;
    bool ok = ccm_star_seal(&aes, nonce, aad, hdr_len + aux_len, payload, len, cipher, cipher + len, 4, true);
    aes128_clear(&aes);
    if (!ok) return 0;
    for (size_t i = 0; i < hdr_len; i++) out[i] = aad[i];
    for (size_t i = 0; i < aux_len; i++) out[hdr_len + i] = aux_bytes[i];
    return total;
}

bool aps_open(const uint8_t key[16], uint8_t *frame, size_t len, size_t hdr_len, nwk_aux_t *aux,
              size_t *payload_offset, size_t *payload_len) {
    return aps_open_from(key, frame, len, hdr_len, 0, aux, payload_offset, payload_len);
}

bool aps_open_from(const uint8_t key[16], uint8_t *frame, size_t len, size_t hdr_len, uint64_t sender,
                   nwk_aux_t *aux, size_t *payload_offset, size_t *payload_len) {
    if (!key || !frame || !aux || hdr_len < 2 || hdr_len >= len || len > APS_MAX_FRAME || !(frame[0] & 0x20u))
        return false;
    if (nwk_aux_parse(frame + hdr_len, len - hdr_len, aux) != NWK_OK || aux->level != 0) return false;
    if (!aux->extended_nonce) {
        if (!sender) return false;
        aux->source = sender;   /* the nonce's address, known from the NWK source */
    }
    size_t offset = hdr_len + aux->len;
    if (len < offset + 4u) return false;
    size_t plen = len - offset - 4u;
    uint8_t aad[APS_MAX_FRAME];
    for (size_t i = 0; i < offset; i++) aad[i] = frame[i];
    aad[hdr_len] = real_sc(aux);
    uint8_t nonce[13];
    nwk_nonce(aux->source, aux->frame_counter, real_sc(aux), nonce);
    aes128_ctx_t aes;
    aes128_init(&aes, key);
    bool ok = ccm_star_open(&aes, nonce, aad, offset, frame + offset, plen, frame + offset,
                            frame + offset + plen, 4, true);
    aes128_clear(&aes);
    if (!ok) return false;
    aux->level = APS_SECURITY_LEVEL;
    if (payload_offset) *payload_offset = offset;
    if (payload_len) *payload_len = plen;
    return true;
}
