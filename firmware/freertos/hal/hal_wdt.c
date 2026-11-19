/* SPDX-License-Identifier: MIT
 * CC26x2 watchdog: first expiry raises a maskable interrupt (records which
 * quorum members were missing, then resets); if even that ISR cannot run,
 * the second expiry resets the device in hardware.
 */
#include "hal_wdt.h"
#include "board.h"
#include "crash_log.h"

#include <inc/hw_ints.h>
#include <inc/hw_memmap.h>
#include <inc/hw_types.h>
#include <driverlib/interrupt.h>
#include <driverlib/watchdog.h>

/* WDT clock = MCU clock / 32 = 1.5 MHz. */
#define WDT_TICKS_PER_MS 1500u

static volatile uint32_t missing_snapshot;

void hal_wdt_start(uint32_t timeout_ms) {
    WatchdogUnlock();
    WatchdogReloadSet(timeout_ms * WDT_TICKS_PER_MS);
    WatchdogIntTypeSet(WATCHDOG_INT_TYPE_INT);
    WatchdogResetEnable();
    WatchdogStallEnable();   /* frozen while a debugger halts the CPU */
    WatchdogIntClear();
    WatchdogEnable();
    WatchdogLock();
    IntPrioritySet(INT_WDT_IRQ, BOARD_IRQ_PRIO_WDT);
    IntPendClear(INT_WDT_IRQ);
    IntEnable(INT_WDT_IRQ);
}

void hal_wdt_feed(void) {
    WatchdogUnlock();
    WatchdogIntClear();      /* clears the interrupt and reloads the counter */
    WatchdogLock();
}

void hal_wdt_note_missing(uint32_t mask) { missing_snapshot = mask; }

void WDT_IRQHandler(void) {
    crash_log_record_text(CRASH_WATCHDOG, "wdt", missing_snapshot);
}
