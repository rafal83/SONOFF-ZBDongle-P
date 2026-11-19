/* SPDX-License-Identifier: MIT
 * Clock and power-domain bring-up without TI-RTOS/Power driver.
 * Calls only BSD TI driverlib functions.
 */
#include "hal_sys.h"
#include "crash_log.h"

#include <inc/hw_memmap.h>
#include <inc/hw_fcfg1.h>
#include <inc/hw_types.h>
#include <driverlib/osc.h>
#include <driverlib/prcm.h>
#include <driverlib/sys_ctrl.h>

/* ~1.5 s at 48 MHz with a few cycles per iteration: a stuck oscillator or
 * power domain yields a recorded failure instead of an endless hang. */
#define HAL_WAIT_BUDGET 20000000u

static uint32_t reset_source;

static int wait_hf_ready(void) {
    for (uint32_t i = 0; i < HAL_WAIT_BUDGET; i++) if (OSCHfSourceReady()) return 1;
    return 0;
}

uint32_t hal_sys_init(void) {
    reset_source = SysCtrlResetSourceGet();
    /* HF XOSC (48 MHz crystal) is required by the radio. */
    if (OSCClockSourceGet(OSC_SRC_CLK_HF) != OSC_XOSC_HF) {
        OSCClockSourceSet(OSC_SRC_CLK_HF, OSC_XOSC_HF);
        if (!wait_hf_ready()) return 1;
        OSCHfSourceSwitch();
    }
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL);
    uint32_t i = 0;
    while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_SERIAL) != PRCM_DOMAIN_POWER_ON) {
        if (++i >= HAL_WAIT_BUDGET) return 2;
    }
    /* Clock gates exist per CPU mode. The idle task executes WFI (sleep
     * mode): without the SLEEP gates the UART loses its clock mid-character
     * and RX can no longer wake the host task. Observed on hardware in the
     * 2026-11-18 lab session r2; TI's Power driver sets all three modes. */
    static const uint32_t peripherals[] = {PRCM_PERIPH_UART0, PRCM_PERIPH_GPIO,
                                           PRCM_PERIPH_TRNG, PRCM_PERIPH_CRYPTO};
    for (unsigned k = 0; k < sizeof peripherals / sizeof peripherals[0]; k++) {
        PRCMPeripheralRunEnable(peripherals[k]);
        PRCMPeripheralSleepEnable(peripherals[k]);
        PRCMPeripheralDeepSleepEnable(peripherals[k]);
    }
    PRCMLoadSet();
    for (i = 0; !PRCMLoadGet(); i++) if (i >= HAL_WAIT_BUDGET) return 3;
    return 0;
}

void hal_sys_ieee_address(uint8_t out_le[8]) {
    /* Factory primary IEEE address. CCFG secondary address is not used. */
    uint32_t lo = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_0);
    uint32_t hi = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_1);
    for (unsigned i = 0; i < 4; i++) {
        out_le[i] = (uint8_t)(lo >> (8u * i));
        out_le[i + 4] = (uint8_t)(hi >> (8u * i));
    }
}

uint32_t hal_sys_reset_source(void) { return reset_source; }

_Noreturn void hal_sys_reset(void) {
    crash_log_record_text(CRASH_REQUESTED_RESET, "host", 0);
}

void hal_cycle_counter_init(void) {
    *(volatile uint32_t *)0xE000EDFCu |= (1u << 24); /* DEMCR.TRCENA */
    *(volatile uint32_t *)0xE0001004u = 0;            /* DWT_CYCCNT */
    *(volatile uint32_t *)0xE0001000u |= 1u;          /* DWT_CTRL.CYCCNTENA */
}
