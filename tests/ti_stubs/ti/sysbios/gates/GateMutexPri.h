/* Test-only single-thread mutex model; not linked into firmware. */
#ifndef TEST_GATE_H
#define TEST_GATE_H
#include <stdint.h>
typedef void *GateMutexPri_Handle;
typedef struct { int unused; } GateMutexPri_Params;
extern unsigned test_gate_depth;
static inline void GateMutexPri_Params_init(GateMutexPri_Params *p) { p->unused = 0; }
static inline GateMutexPri_Handle GateMutexPri_create(GateMutexPri_Params *p, void *e) { (void)p; (void)e; return (void *)1; }
static inline int32_t GateMutexPri_enter(GateMutexPri_Handle g) { (void)g; return (int32_t)test_gate_depth++; }
static inline void GateMutexPri_leave(GateMutexPri_Handle g, int32_t key) { (void)g; test_gate_depth = (unsigned)key; }
#endif
