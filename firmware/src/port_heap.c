/* SPDX-License-Identifier: MIT
 * Serialized wrapper around the separately licensed BSD TLSF allocator.
 */
#include "port_heap.h"
#include "tlsf.h"
#ifdef PORT_HEAP_HOST_TEST
#include <stdlib.h>
#endif
volatile uint32_t port_heap_faults;
_Noreturn void port_heap_fault(void) {
    port_heap_faults++;
#ifdef PORT_HEAP_HOST_TEST
    abort();
#else
    for (;;) { /* Heap corruption is fatal; do not continue with invalid state. */ }
#endif
}
static tlsf_t heap;
static size_t total;
static size_t charged;
static uint32_t allocations, failures;
bool port_heap_init(void *memory, size_t bytes) {
    if (heap) return true; /* Never discard live allocations on a repeated init. */
    if (!memory || (uintptr_t)memory % tlsf_align_size() || bytes <= tlsf_size() + tlsf_pool_overhead() ||
        bytes - tlsf_size() - tlsf_pool_overhead() >= tlsf_block_size_max()) return false;
    heap = tlsf_create_with_pool(memory, bytes);
    if (!heap) return false;
    total = bytes - tlsf_size() - tlsf_pool_overhead();
    return true;
}
void *port_heap_alloc(size_t bytes, size_t alignment) {
    if (!heap || !bytes || !alignment || (alignment & (alignment - 1)) || alignment > 256) return NULL;
    if (bytes >= tlsf_block_size_max()) { failures++; return NULL; }
    void *p = alignment <= tlsf_align_size() ? tlsf_malloc(heap, bytes) : tlsf_memalign(heap, alignment, bytes);
    if (p) { allocations++; charged += tlsf_block_size(p) + 2 * tlsf_alloc_overhead(); } else failures++;
    return p;
}
void *port_heap_realloc(void *p, size_t bytes) {
    if (!heap) return NULL;
    if (bytes >= tlsf_block_size_max()) { failures++; return NULL; }
    size_t previous = p ? tlsf_block_size(p) + 2 * tlsf_alloc_overhead() : 0;
    void *result = tlsf_realloc(heap, p, bytes);
    if (result || !bytes) {
        charged -= previous;
        if (result) charged += tlsf_block_size(result) + 2 * tlsf_alloc_overhead();
    }
    if (bytes && !result) failures++;
    return result;
}
void port_heap_free(void *p) {
    if (heap && p) { charged -= tlsf_block_size(p) + 2 * tlsf_alloc_overhead(); tlsf_free(heap, p); }
}
size_t port_heap_free_budget(void) { return total > charged ? total - charged : 0; }
static void stats_walker(void *p, size_t bytes, int used, void *context) {
    (void)p;
    port_heap_stats *s = context;
    if (!used) { s->free += bytes; if (bytes > s->largest) s->largest = bytes; }
}
void port_heap_get_stats(port_heap_stats *s) {
    *s = (port_heap_stats){ .total = total, .allocations = allocations, .failures = failures };
    if (heap) tlsf_walk_pool(tlsf_get_pool(heap), stats_walker, s);
}
int port_heap_check(void) { return heap ? tlsf_check(heap) | tlsf_check_pool(tlsf_get_pool(heap)) : -1; }
