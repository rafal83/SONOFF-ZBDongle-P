#ifndef TEST_WATCHDOG_H
#define TEST_WATCHDOG_H
#include <stddef.h>
#include <stdint.h>
typedef void *Watchdog_Handle;
typedef struct {
    void (*callbackFxn)(uintptr_t);
    unsigned resetMode, debugStallMode;
} Watchdog_Params;
#define Watchdog_RESET_ON 1u
#define Watchdog_DEBUG_STALL_ON 1u
void Watchdog_init(void);
void Watchdog_Params_init(Watchdog_Params *);
Watchdog_Handle Watchdog_open(unsigned, Watchdog_Params *);
void Watchdog_clear(Watchdog_Handle);
#endif
