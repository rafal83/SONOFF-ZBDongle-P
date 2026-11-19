/* SPDX-License-Identifier: MIT */
#include "autobaud.h"
#include <stddef.h>
uint32_t port_autobaud_estimate(const uint32_t edges[4], uint32_t hz) {
    static const uint32_t rates[] = {115200, 460800, 1000000};
    if (!edges || !hz) return 0;
    uint32_t low = edges[1] - edges[0], total = edges[2] - edges[0];
    uint32_t next_low = edges[3] - edges[2];
    if (!low || low >= total || total > hz / 100u || !next_low || next_low > hz / 100u) return 0;
    uint32_t selected = 0;
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        uint64_t l = (uint64_t)low * rates[i], t = (uint64_t)total * rates[i];
        uint64_t nominal_low = (uint64_t)hz * 2u, nominal_total = (uint64_t)hz * 10u;
        uint64_t n = (uint64_t)next_low * rates[i], nominal_next = (uint64_t)hz * 9u;
        /* Allow per-edge bus arbitration jitter, but require the complete
         * start-to-start interval to match to 10%. Rates cannot overlap. */
        if (l * 5u < nominal_low * 4u || l * 5u > nominal_low * 6u ||
            t * 10u < nominal_total * 9u || t * 10u > nominal_total * 11u ||
            n * 10u < nominal_next * 9u || n * 10u > nominal_next * 11u) continue;
        if (selected) return 0;
        selected = rates[i];
    }
    return selected;
}
