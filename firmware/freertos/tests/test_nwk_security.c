/* SPDX-License-Identifier: MIT
 * Existing NWK security engine (firmware/src/core_security.c) driven by the
 * new software CCM* backend, against a frame sealed by pyca/cryptography.
 */
#include "frt_test.h"
#include "core_security.h"
#include "zb_ccm_backend.h"
#include "nwk_frame.h"
#include "vectors_crypto.h"

static uint32_t reserved_upper;
static int reserve_calls;
static bool reserve(void *ctx, uint32_t upper) {
    (void)ctx;
    reserve_calls++;
    reserved_upper = upper;
    return true;
}

static core_security_t rx, tx;

int main(void) {
    core_security_init(&rx, 0, zb_ccm_backend_sw, reserve, 0);
    CHECK(core_security_set_key(&rx, 0, 0, NWK_KEY));
    uint8_t frame[128];
    size_t payload_len = 0;
    memcpy(frame, NWK_SECURED_FRAME, sizeof NWK_SECURED_FRAME);
    /* Unknown peer: refused by the plain open, admitted after authentication. */
    CHECK(!core_security_open(&rx, frame, sizeof NWK_SECURED_FRAME, NWK_SECURED_HEADER_LEN, &payload_len));
    memcpy(frame, NWK_SECURED_FRAME, sizeof NWK_SECURED_FRAME);
    CHECK(core_security_open_authenticated_peer(&rx, frame, sizeof NWK_SECURED_FRAME,
                                                NWK_SECURED_HEADER_LEN, &payload_len));
    CHECK(payload_len == sizeof NWK_PLAINTEXT);
    CHECK(!memcmp(frame + NWK_SECURED_HEADER_LEN + 14, NWK_PLAINTEXT, sizeof NWK_PLAINTEXT));
    /* Replay of the same counter is refused. */
    memcpy(frame, NWK_SECURED_FRAME, sizeof NWK_SECURED_FRAME);
    CHECK(!core_security_open(&rx, frame, sizeof NWK_SECURED_FRAME, NWK_SECURED_HEADER_LEN, &payload_len));
    /* Tampering in header, aux, payload or MIC is refused. */
    core_security_t fresh;
    core_security_init(&fresh, 0, zb_ccm_backend_sw, reserve, 0);
    CHECK(core_security_set_key(&fresh, 0, 0, NWK_KEY));
    for (size_t i = 0; i < sizeof NWK_SECURED_FRAME; i++) {
        if (i == 1) continue; /* option bits change the parsed header layout */
        memcpy(frame, NWK_SECURED_FRAME, sizeof NWK_SECURED_FRAME);
        frame[i] ^= 0x01;
        CHECK(!core_security_open_authenticated_peer(&fresh, frame, sizeof NWK_SECURED_FRAME,
                                                     NWK_SECURED_HEADER_LEN, &payload_len));
    }
    /* Wrong key. */
    uint8_t wrong[16];
    memcpy(wrong, NWK_KEY, 16);
    wrong[0] ^= 0x80;
    core_security_init(&fresh, 0, zb_ccm_backend_sw, reserve, 0);
    CHECK(core_security_set_key(&fresh, 0, 0, wrong));
    memcpy(frame, NWK_SECURED_FRAME, sizeof NWK_SECURED_FRAME);
    CHECK(!core_security_open_authenticated_peer(&fresh, frame, sizeof NWK_SECURED_FRAME,
                                                 NWK_SECURED_HEADER_LEN, &payload_len));

    /* Coordinator seals, the peer opens: build the header with nwk_build. */
    core_security_init(&tx, 5000, zb_ccm_backend_sw, reserve, 0);
    CHECK(core_security_set_key(&tx, 0, 0, NWK_KEY));
    const uint8_t local[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    for (unsigned round = 0; round < 3000; round++) {
        nwk_header_t h = {0};
        h.type = NWK_TYPE_DATA;
        h.dst = 0x4E21; h.src = 0x0000; h.radius = 30; h.seq = (uint8_t)round;
        h.has_src_ieee = true;
        h.src_ieee = 0x0807060504030201ull;
        size_t hl = nwk_build(&h, frame, sizeof frame);
        CHECK(hl == 16);
        size_t pl = 1 + round % 60, total = 0;
        for (size_t k = 0; k < pl; k++) frame[hl + k] = (uint8_t)(k ^ round);
        CHECK(core_security_seal(&tx, 0, local, frame, hl, pl, sizeof frame, &total));
        CHECK(total == hl + 14 + pl + 4);
        nwk_aux_t aux;
        CHECK(nwk_aux_parse(frame + hl, total - hl, &aux) == NWK_OK && aux.level == 0);
        CHECK(aux.frame_counter == 5000u + round);
        size_t got = 0;
        CHECK(core_security_open_authenticated_peer(&rx, frame, total, hl, &got) && got == pl);
        for (size_t k = 0; k < pl; k++) CHECK(frame[hl + 14 + k] == (uint8_t)(k ^ round));
    }
    /* Durable reservation happened in windows of CORE_SECURITY_RESERVATION. */
    CHECK(reserve_calls >= 3 && reserved_upper == 5000u + 3u * CORE_SECURITY_RESERVATION);
    PASS("nwk_security");
    return 0;
}
