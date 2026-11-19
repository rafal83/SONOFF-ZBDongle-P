/* SPDX-License-Identifier: MIT */
#ifndef PORT_HEAP_H
#define PORT_HEAP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { size_t total, free, largest; uint32_t allocations, failures; } port_heap_stats;
bool port_heap_init(void *memory, size_t bytes);
void *port_heap_alloc(size_t bytes, size_t alignment);
void *port_heap_realloc(void *pointer, size_t bytes);
void port_heap_free(void *pointer);
void port_heap_get_stats(port_heap_stats *stats);
size_t port_heap_free_budget(void);
int port_heap_check(void);
_Noreturn void port_heap_fault(void);
/* Caller serializes every API, including stats; no internal OS dependency. */
#endif
