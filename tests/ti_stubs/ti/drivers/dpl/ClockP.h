#ifndef TEST_CLOCK_P_H
#define TEST_CLOCK_P_H
#include <stdint.h>
#include <stdbool.h>
uint64_t ClockP_getSystemTicks64(void);
uint32_t ClockP_getSystemTickPeriod(void);
typedef void *ClockP_Handle;
typedef void (*ClockP_Fxn)(uintptr_t);
typedef union { uint64_t align; uint8_t data[96]; } ClockP_Struct;
typedef struct { uintptr_t arg; uint32_t period; bool startFlag; } ClockP_Params;
void ClockP_Params_init(ClockP_Params *);
ClockP_Handle ClockP_construct(ClockP_Struct *, ClockP_Fxn, uint32_t, ClockP_Params *);
void ClockP_destruct(ClockP_Struct *);
void ClockP_stop(ClockP_Handle);
void ClockP_start(ClockP_Handle);
void ClockP_setTimeout(ClockP_Handle, uint32_t);
void ClockP_setPeriod(ClockP_Handle, uint32_t);
uint32_t ClockP_getTimeout(ClockP_Handle);
#endif
