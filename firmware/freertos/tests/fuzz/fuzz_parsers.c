/* SPDX-License-Identifier: MIT
 * libFuzzer: every decoder of untrusted bytes, on the raw input.
 *  - IEEE 802.15.4 MAC frame, Zigbee beacon payload;
 *  - NWK header, NWK auxiliary security header, APS header;
 *  - MT/NPI serial parser (bytes from the host UART).
 * Built with -fsanitize=fuzzer,address,undefined by tools/fuzz_freertos.py.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "aps_frame.h"
#include "mac_frame.h"
#include "npi.h"
#include "nwk_frame.h"

static void on_npi(void *ctx, const npi_frame *frame) { (void)ctx; (void)frame; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mac_frame_t f;
    if (mac_frame_parse(data, size, &f) == MAC_OK) {
        mac_zigbee_beacon_t b;
        (void)mac_parse_zigbee_beacon(&f, &b);
        /* Rebuild what was parsed: the builder must accept its own output. */
        uint8_t out[MAC_MAX_MPDU];
        (void)mac_frame_build(&f, f.payload, f.payload_len, out, sizeof out);
    }
    nwk_header_t h;
    if (nwk_parse(data, size, &h) == NWK_OK && h.header_len <= size) {
        nwk_aux_t aux;
        (void)nwk_aux_parse(data + h.header_len, size - h.header_len, &aux);
        aps_header_t a;
        (void)aps_parse(data + h.header_len, size - h.header_len, &a);
        uint8_t out[128];
        (void)nwk_build(&h, out, sizeof out);
    }
    aps_header_t a;
    (void)aps_parse(data, size, &a);
    npi_parser p;
    npi_init(&p, on_npi, 0);
    uint32_t now = 0;
    for (size_t i = 0; i < size; i++) {
        npi_feed(&p, data[i], now);
        now += data[i] & 0x3Fu;   /* inter-byte gaps also come from the input */
        if ((data[i] & 0xC0u) == 0xC0u) npi_expire(&p, now + 200);
    }
    return 0;
}
