/* SPDX-License-Identifier: MIT */
#include "frt_test.h"
#include "spsc_ring.h"

int main(void) {
    uint8_t storage[16], out[32];
    spsc_ring_t r;
    CHECK(!spsc_ring_init(&r, storage, 0));
    CHECK(!spsc_ring_init(&r, storage, 12));
    CHECK(spsc_ring_init(&r, storage, 16));
    CHECK(spsc_ring_free(&r) == 16 && spsc_ring_used(&r) == 0);
    uint8_t byte;
    CHECK(!spsc_ring_get(&r, &byte));
    for (unsigned i = 0; i < 16; i++) CHECK(spsc_ring_put(&r, (uint8_t)i));
    CHECK(!spsc_ring_put(&r, 99));
    CHECK(spsc_ring_read(&r, out, 32) == 16);
    for (unsigned i = 0; i < 16; i++) CHECK(out[i] == i);
    /* Free-running indices across the 32-bit wrap. */
    r.head = r.tail = 0xFFFFFFF0u;
    uint8_t seq = 0, expect = 0;
    for (unsigned round = 0; round < 1000; round++) {
        unsigned n = frt_rand() % 17;
        uint8_t in[17];
        for (unsigned i = 0; i < n; i++) in[i] = seq++;
        size_t put = spsc_ring_write(&r, in, n);
        seq = (uint8_t)(seq - (n - put));
        CHECK(spsc_ring_used(&r) <= 16);
        size_t got = spsc_ring_read(&r, out, frt_rand() % 17);
        for (size_t i = 0; i < got; i++) CHECK(out[i] == expect++);
    }
    CHECK(r.head < 0xFFFFFFF0u); /* wrapped */
    PASS("spsc_ring");
    return 0;
}
