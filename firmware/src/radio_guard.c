/* SPDX-License-Identifier: MIT
 * Independent bounds checks using TI public header formats and Zigbee R23
 * sections 3.3.1 and 4.3.1. Does not replace the TI security implementation.
 */
#include "radio_guard.h"
uint32_t port_radio_rejected;
uint32_t port_radio_last_rejection;
bool port_radio_guard_accept(const uint8_t *p, size_t size, uint8_t security_level) {
    unsigned reason = 1;
    if (!p || size < 2 || size > 127) goto reject;
    unsigned type = p[0] & 3u, version = (p[0] >> 2) & 15u;
    /* Native callback reads two FC bytes even before dispatching Green Power.
     * GP has a distinct header: leave its parsing to the native GP path.
     */
    if (version == 3 && type <= 1) return true;
    if (version != 2 || type == 2) { reason = 2; goto reject; }
    if (type == 3) {
        /* Stub APS callback reads its FC before the ordinary NWK filter. */
        if (size < 3) goto reject;
        if ((p[2] & 3u) != 3u) return true; /* Native callback rejects it. */
        size_t required = (p[2] & 12u) == 12u ? 9u : 7u;
        if (size < required) goto reject;
        return true;
    }
    if (size < 8) goto reject;
    unsigned options = p[1];
    size_t header = 8;
    if (options & 8u) header += 8;  /* Destination IEEE address. */
    if (options & 16u) header += 8; /* Source IEEE address. */
    if (options & 1u) header++;    /* Multicast control. */
    if (header > size) goto reject;
    if (options & 4u) {
        if (size - header < 2) goto reject;
        unsigned count = p[header], index = p[header + 1];
        /* A source route needs a nonempty relay list and an in-range index. */
        if (!count || index >= count) { reason = 5; goto reject; }
        header += 2u + 2u * count;
        if (header > size) goto reject;
    }
    if (options & 2u) {
        /* NWK security always uses the network key and an extended nonce.
         * The on-wire security level is overwritten from the NIB by TI.
         */
        if (security_level > 7 || size - header < 14) { reason = 6; goto reject; }
        if ((p[header] & 0x38u) != 0x28u) { reason = 7; goto reject; }
        static const uint8_t mic[8] = {0, 4, 8, 16, 0, 4, 8, 16};
        header += 14u + mic[security_level];
    }
    /* Data must contain an APS FC; a NWK command must contain its ID.
     * Encrypted APS contents are not inspected here.
     */
    if (header >= size) { reason = 8; goto reject; }
    return true;
reject:
    port_radio_rejected++;
    port_radio_last_rejection = (uint32_t)(size < 256 ? size : 255) | (reason << 8);
    if (p && size >= 2 && size <= 127)
        port_radio_last_rejection |= ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 24);
    return false;
}
