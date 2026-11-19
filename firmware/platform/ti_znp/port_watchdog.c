/* SPDX-License-Identifier: MIT
 * The NPI and stack task loops must both progress before the WDT is fed.
 * Never feed from a timer/ISR: those can continue during a task deadlock.
 */
#include "port_watchdog.h"
#include "osal_port.h"
#include <ti/drivers/Watchdog.h>
#include <ti/drivers/dpl/ClockP.h>
#include <ti_drivers_config.h>
static Watchdog_Handle watchdog;
static uint8_t progress;
static volatile uint8_t expired;
static void timeout(uintptr_t handle) {
    (void)handle;
    /* NMI context. No BIOS, driver, flash, heap or logging calls are safe.
     * Latch the failure so resumed tasks cannot hide the missed deadline.
     * CC26XX resets on the second expiry with its interrupt still pending.
     */
    expired = 1;
}
bool PortWatchdog_start(void) {
    if (watchdog) return true; /* Stack task calls once, after initialization. */
    Watchdog_Params params;
    Watchdog_init();
    Watchdog_Params_init(&params);
    params.resetMode = Watchdog_RESET_ON;
    params.debugStallMode = Watchdog_DEBUG_STALL_ON;
    params.callbackFxn = timeout;
    progress = 0;
    watchdog = Watchdog_open(CONFIG_PORT_WATCHDOG, &params);
    return watchdog != NULL;
}
void PortWatchdog_progress(uint8_t participant) {
    if (participant != PORT_WATCHDOG_NPI && participant != PORT_WATCHDOG_STACK) return;
    uint32_t key = OsalPort_enterCS();
    if (watchdog && !expired) {
        progress |= participant;
        if (progress == (PORT_WATCHDOG_NPI | PORT_WATCHDOG_STACK)) {
            progress = 0;
            Watchdog_clear(watchdog);
        }
    }
    OsalPort_leaveCS(key);
}
uint32_t PortWatchdog_waitTicks(uint32_t requested) {
    /* Wake an otherwise idle task once per second. No timer or heap object. */
    uint32_t period = ClockP_getSystemTickPeriod();
    uint32_t limit = (1000000u + period - 1u) / period;
    return requested < limit ? requested : limit;
}
