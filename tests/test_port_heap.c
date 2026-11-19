#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "port_heap.h"
#include "tlsf.h"
static _Alignas(256) uint8_t arena[32768];
static uint32_t rng = 0x12345678;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
typedef struct { void *p; size_t size; uint8_t value; } allocation;
static void verify(const allocation *a) {
    if (a->p) for (size_t i = 0; i < a->size; i++) assert(((uint8_t *)a->p)[i] == a->value);
}
int main(void) {
    assert(!port_heap_init(NULL, sizeof(arena)));
    assert(port_heap_init(arena, sizeof(arena)));
    port_heap_stats initial, final;
    port_heap_get_stats(&initial);
    assert(initial.free && initial.largest == initial.free && !port_heap_check());
    for (size_t align = 4; align <= 256; align *= 2) {
        void *p = port_heap_alloc(73, align); assert(p && !((uintptr_t)p % align)); port_heap_free(p);
    }
    assert(!port_heap_alloc(16, 3) && !port_heap_alloc(SIZE_MAX, 8));
    allocation active[128] = {0};
    for (unsigned iteration = 0; iteration < 200000; iteration++) {
        unsigned slot = random32() % 128;
        allocation *a = &active[slot]; verify(a);
        if (a->p) {
            if (random32() & 1) {
                size_t size = 1 + random32() % 1024;
                void *p = port_heap_realloc(a->p, size);
                if (p) {
                    assert(!((uintptr_t)p % 8));
                    allocation old = {p, size < a->size ? size : a->size, a->value}; verify(&old);
                    a->p = p; a->size = size; memset(p, a->value, size);
                } else verify(a);
            } else { port_heap_free(a->p); *a = (allocation){0}; }
        } else {
            a->size = 1 + random32() % 1024; a->value = (uint8_t)random32();
            a->p = port_heap_alloc(a->size, 8);
            if (a->p) { assert(!((uintptr_t)a->p % 8)); memset(a->p, a->value, a->size); }
        }
        if (iteration % 1000 == 0) {
            for (unsigned j = 0; j < 128; j++) verify(&active[j]);
            assert(!port_heap_check());
        }
    }
    for (unsigned j = 0; j < 128; j++) { verify(&active[j]); port_heap_free(active[j].p); }
    void *blocks[2048]; unsigned count = 0;
    while (count < 2048 && (blocks[count] = port_heap_alloc(1, 8))) count++;
    assert(count && count < 2048 && !port_heap_alloc(1, 8));
    /* Free alternating blocks, then all of them: both physical neighbours merge. */
    for (unsigned j = 0; j < count; j += 2) port_heap_free(blocks[j]);
    assert(!port_heap_check());
    for (unsigned j = 1; j < count; j += 2) port_heap_free(blocks[j]);
    port_heap_get_stats(&final);
    assert(final.free == initial.free && final.largest == initial.largest && !port_heap_check());
    assert(port_heap_free_budget() == initial.total);
    void *p = port_heap_alloc(20, 8); memset(p, 0xa5, 20);
    assert(port_heap_init(arena, sizeof(arena))); /* Repeat init preserves live memory. */
    allocation a = {p, 20, 0xa5}; verify(&a); port_heap_free(p);
    printf("PASS: TLSF global heap, 200000 randomized operations, realloc preservation, alignments 4..256, saturation/coalescing, full recovery; control=%zu bytes\n", tlsf_size());
    return 0;
}
