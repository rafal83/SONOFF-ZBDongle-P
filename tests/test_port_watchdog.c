/* Test the actual task quorum and permanent timeout latch, without hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "port_watchdog.h"
#include <ti/drivers/Watchdog.h>
static unsigned cs, feeds, opens, inits;
static int reject = 1;
static int expire_during_clear;
static uint32_t tick_period = 10;
static void (*nmi)(uintptr_t);
uint32_t OsalPort_enterCS(void) { return cs++; }
void OsalPort_leaveCS(uint32_t key) { assert(cs == key + 1); cs = key; }
uint32_t ClockP_getSystemTickPeriod(void) { return tick_period; }
void Watchdog_init(void) { inits++; }
void Watchdog_Params_init(Watchdog_Params *p) { memset(p, 0, sizeof(*p)); }
Watchdog_Handle Watchdog_open(unsigned index, Watchdog_Params *p) {
    assert(index == 0 && p->resetMode == Watchdog_RESET_ON);
    assert(p->debugStallMode == Watchdog_DEBUG_STALL_ON && p->callbackFxn);
    opens++; nmi = p->callbackFxn; return reject ? NULL : (void *)1;
}
void Watchdog_clear(Watchdog_Handle h) {
    assert(cs && h == (void *)1);
    if (expire_during_clear) nmi(1); /* NMI can preempt an in-flight feed. */
    feeds++;
}
int main(void) {
    PortWatchdog_progress(PORT_WATCHDOG_NPI);
    PortWatchdog_progress(PORT_WATCHDOG_STACK);
    assert(!feeds);
    assert(!PortWatchdog_start());
    reject = 0; assert(PortWatchdog_start());
    assert(PortWatchdog_start() && opens == 2 && inits == 2);
    for (unsigned i = 0; i < 1000; i++) PortWatchdog_progress(PORT_WATCHDOG_NPI);
    assert(!feeds); /* Busy NPI cannot mask a stalled Zigbee task. */
    PortWatchdog_progress(0); PortWatchdog_progress(3); PortWatchdog_progress(255);
    assert(!feeds);
    PortWatchdog_progress(PORT_WATCHDOG_STACK); assert(feeds == 1);
    for (unsigned i = 0; i < 1000; i++) PortWatchdog_progress(PORT_WATCHDOG_STACK);
    assert(feeds == 1); /* The reverse stall is detected as well. */
    PortWatchdog_progress(PORT_WATCHDOG_NPI); assert(feeds == 2);
    for (unsigned i = 0; i < 1000; i++) {
        PortWatchdog_progress(PORT_WATCHDOG_NPI);
        PortWatchdog_progress(PORT_WATCHDOG_STACK);
    }
    assert(feeds == 1002);
    assert(PortWatchdog_waitTicks(UINT32_MAX) == 100000);
    assert(PortWatchdog_waitTicks(17) == 17 && !PortWatchdog_waitTicks(0));
    tick_period = 7; assert(PortWatchdog_waitTicks(UINT32_MAX) == 142858);
    expire_during_clear = 1;
    PortWatchdog_progress(PORT_WATCHDOG_NPI);
    PortWatchdog_progress(PORT_WATCHDOG_STACK);
    assert(feeds == 1003); /* One already-started feed may complete. */
    nmi(1); /* A task resuming after the deadline cannot keep feeding. */
    for (unsigned i = 0; i < 1000; i++) {
        PortWatchdog_progress(PORT_WATCHDOG_NPI);
        PortWatchdog_progress(PORT_WATCHDOG_STACK);
    }
    assert(feeds == 1003 && !cs);
    puts("PASS: task-quorum watchdog, both stall directions, idle wait bounds, startup failure, NMI timeout latch prevents late feeds");
    return 0;
}
