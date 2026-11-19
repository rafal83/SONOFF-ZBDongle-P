/* SPDX-License-Identifier: MIT */
#ifndef FRT_HAL_SYS_H
#define FRT_HAL_SYS_H
#include <stdint.h>

/* Bounded busy-waits: each returns 0 on success, a non-zero step id when a
 * hardware condition did not occur within its budget. */
uint32_t hal_sys_init(void);
void hal_sys_ieee_address(uint8_t out_le[8]);
/* RSTSRC_* value captured at boot (see driverlib sys_ctrl.h). */
uint32_t hal_sys_reset_source(void);
_Noreturn void hal_sys_reset(void);
/* Free-running 48 MHz DWT cycle counter. */
void hal_cycle_counter_init(void);
static inline uint32_t hal_cycles(void) { return *(volatile uint32_t *)0xE0001004u; }
#endif
