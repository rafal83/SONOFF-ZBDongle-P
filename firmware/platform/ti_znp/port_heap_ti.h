/* SPDX-License-Identifier: MIT
 * Included by the TI OSAL port in place of its linear heap template.
 * Function signatures preserve TI-RTOS HeapCallback and OSAL contracts.
 */
#include "port_heap.h"
#include <ti/sysbios/knl/Queue.h>
#include <ti/sysbios/heaps/HeapCallback.h>
#include <ti/sysbios/heaps/HeapMem.h>
#undef HEAPMGR_SIZE
uint32_t HEAPMGR_SIZE;
#if defined(PORT_EXPERIMENTAL_CORES)
extern bool PortCore_reserveHeap(void);
#endif
void OsalPort_heapInit(void) {
    extern uint8_t heapStart, heapEnd;
    uintptr_t begin = ((uintptr_t)&heapStart + 7u) & ~(uintptr_t)7u;
    uintptr_t end = (uintptr_t)&heapEnd & ~(uintptr_t)7u;
    HEAPMGR_LOCK();
    HEAPMGR_SIZE = end > begin ? (uint32_t)(end - begin) : 0;
    (void)port_heap_init((void *)begin, HEAPMGR_SIZE);
#if defined(PORT_EXPERIMENTAL_CORES)
    (void)PortCore_reserveHeap();
#endif
    HEAPMGR_UNLOCK();
}
static void *OsalPort_heapMalloc(uint32_t size) {
    HEAPMGR_LOCK(); void *p = port_heap_alloc(size, 8); HEAPMGR_UNLOCK(); return p;
}
static void *OsalPort_heapRealloc(void *p, uint32_t size) {
    HEAPMGR_LOCK(); void *result = port_heap_realloc(p, size); HEAPMGR_UNLOCK(); return result;
}
static void OsalPort_heapFree(void *p) { HEAPMGR_LOCK(); port_heap_free(p); HEAPMGR_UNLOCK(); }
void OsalPort_heapGetStats(ICall_heapStats_t *stats) {
    port_heap_stats s;
    HEAPMGR_LOCK(); port_heap_get_stats(&s); HEAPMGR_UNLOCK();
    stats->totalSize = s.total; stats->totalFreeSize = s.free; stats->largestFreeSize = s.largest;
}
uint32_t HEAPMGR_GETLARGESTFREEBUF(void) {
    port_heap_stats s; HEAPMGR_LOCK(); port_heap_get_stats(&s); HEAPMGR_UNLOCK(); return s.largest;
}
void *OsalPort_heapMallocLimited(uint32_t size) {
    void *result = NULL;
    HEAPMGR_LOCK();
    if (port_heap_free_budget() > (size_t)size + 550u) result = port_heap_alloc(size, 8);
    HEAPMGR_UNLOCK(); return result;
}
UArg osalHeapInitFxn(UArg arg) { (void)arg; OsalPort_heapInit(); return 0xBABE; }
Ptr osalHeapAllocFxn(UArg arg, SizeT size, SizeT alignment) {
    (void)arg; HEAPMGR_LOCK(); void *p = port_heap_alloc(size, alignment ? alignment : 8); HEAPMGR_UNLOCK(); return p;
}
void osalHeapFreeFxn(UArg arg, Ptr p, SizeT size) { (void)arg; (void)size; OsalPort_heapFree(p); }
void osalHeapGetStatsFxn(UArg arg, Memory_Stats *stats) { (void)arg; OsalPort_heapGetStats((ICall_heapStats_t *)stats); }
Bool osalHeapIsBlockingFxn(UArg arg) { (void)arg; return FALSE; }
