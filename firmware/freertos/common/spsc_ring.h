/* SPDX-License-Identifier: MIT
 * Single-producer/single-consumer byte ring, lock-free on a single core.
 * Indices are free-running 32-bit counters; capacity is a power of two.
 * Producer and consumer may run in different contexts (ISR vs task) as long
 * as each side has exactly one caller. Every operation is O(len), no loops
 * over unrelated state, no allocation.
 */
#ifndef FRT_SPSC_RING_H
#define FRT_SPSC_RING_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    uint32_t mask;
    volatile uint32_t head;  /* written by producer only */
    volatile uint32_t tail;  /* written by consumer only */
} spsc_ring_t;

#if defined(__arm__)
#define SPSC_BARRIER() __asm volatile("dmb" ::: "memory")
#else
#define SPSC_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

static inline int spsc_ring_init(spsc_ring_t *r, uint8_t *storage, uint32_t capacity) {
    if (capacity < 2 || (capacity & (capacity - 1u))) return 0;
    r->data = storage;
    r->mask = capacity - 1u;
    r->head = r->tail = 0;
    return 1;
}
static inline uint32_t spsc_ring_used(const spsc_ring_t *r) { return r->head - r->tail; }
static inline uint32_t spsc_ring_free(const spsc_ring_t *r) { return r->mask + 1u - (r->head - r->tail); }

static inline int spsc_ring_put(spsc_ring_t *r, uint8_t byte) {
    uint32_t head = r->head;
    if (head - r->tail > r->mask) return 0;
    r->data[head & r->mask] = byte;
    SPSC_BARRIER();
    r->head = head + 1u;
    return 1;
}
static inline int spsc_ring_get(spsc_ring_t *r, uint8_t *byte) {
    uint32_t tail = r->tail;
    if (r->head == tail) return 0;
    SPSC_BARRIER();
    *byte = r->data[tail & r->mask];
    SPSC_BARRIER();
    r->tail = tail + 1u;
    return 1;
}
static inline size_t spsc_ring_write(spsc_ring_t *r, const uint8_t *src, size_t len) {
    size_t n = 0;
    while (n < len && spsc_ring_put(r, src[n])) n++;
    return n;
}
static inline size_t spsc_ring_read(spsc_ring_t *r, uint8_t *dst, size_t max) {
    size_t n = 0;
    while (n < max && spsc_ring_get(r, &dst[n])) n++;
    return n;
}
#endif
