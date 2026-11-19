/* Exercise fixed dictionary capacity, fingerprint collisions and updates. */
#include <assert.h>
#include <stdio.h>
#include "nv_index.h"
int main(void) {
    uint16_t locations[PORT_NV_INDEX_CANDIDATES];
    for (unsigned round = 0; round < 20; round++) {
        port_nv_index_invalidate(); port_nv_index_begin();
        for (unsigned i = 0; i < 800; i++)
            assert(port_nv_index_put(i * 0x9e3779b1u, (uint16_t)(i + 128), true));
        port_nv_index_finish(true); assert(port_nv_index_ready());
        for (unsigned i = 0; i < 800; i++) {
            assert(port_nv_index_get(i * 0x9e3779b1u, locations) == 1);
            assert(locations[0] == i + 128);
        }
        for (unsigned i = 800; i < 1800; i++)
            assert(!port_nv_index_get(i * 0x9e3779b1u, locations));
        assert(port_nv_index_put(0, 20000, true));
        assert(port_nv_index_get(0, locations) == 1 && locations[0] == 20000);
        port_nv_index_remove(0, 128);
        assert(port_nv_index_get(0, locations) == 1 && locations[0] == 20000);
        port_nv_index_remove(0, 20000); assert(!port_nv_index_get(0, locations));
    }
    port_nv_index_invalidate(); port_nv_index_begin();
    for (unsigned i = 0; i < 6; i++) assert(port_nv_index_put(0x80000123, (uint16_t)(500 + i), false));
    assert(port_nv_index_get(0x80000123, locations) == 6);
    for (unsigned i = 0; i < 6; i++) {
        bool seen = false;
        for (unsigned j = 0; j < 6; j++) if (locations[j] == 500 + i) seen = true;
        assert(seen);
    }
    port_nv_index_remove(0x80000123, 502);
    assert(port_nv_index_get(0x80000123, locations) == 5);
    assert(port_nv_index_put(0x80000123, 502, false));
    /* Full fingerprints are verified by the TI adapter. Too many colliding
     * fingerprints must disable the index rather than lose a record silently. */
    bool full = false;
    for (unsigned i = 6; i < 40; i++) if (!port_nv_index_put(0x80000123, (uint16_t)(500 + i), false)) { full = true; break; }
    assert(full && port_nv_index_blocked() && !port_nv_index_ready());
    port_nv_index_finish(true); assert(!port_nv_index_ready());
    port_nv_index_invalidate(); assert(!port_nv_index_blocked());
    port_nv_index_begin(); assert(!port_nv_index_get(0x80000123, locations));
    assert(port_nv_index_stats.overflows > 0);
    puts("PASS: NV directory, 20 generations of 800 keys, absent keys, updates, equal-fingerprint candidates, capacity failure disables incomplete index");
    return 0;
}
