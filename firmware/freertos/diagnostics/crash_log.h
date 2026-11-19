/* SPDX-License-Identifier: MIT
 * Post-mortem record kept in no-init SRAM across a software/watchdog reset.
 * HARDWARE RESULT (2026-11-18, session r3): after a CPU warm reset
 * (SYSRESETREQ) the record was NOT found valid on the next boot, so SRAM
 * retention cannot be relied on. The record is therefore ALSO emitted on the
 * UART (MT AREQ 0x5F 0x82) just before the reset, which works on hardware.
 * The record is validated by magic and checksum before being reported.
 */
#ifndef FRT_CRASH_LOG_H
#define FRT_CRASH_LOG_H
#include <stdbool.h>
#include <stdint.h>

enum {
    CRASH_NONE = 0,
    CRASH_ASSERT = 1,
    CRASH_STACK_OVERFLOW = 2,
    CRASH_HARD_FAULT = 3,
    CRASH_UNEXPECTED_IRQ = 4,
    CRASH_WATCHDOG = 5,
    CRASH_REQUESTED_RESET = 6,
};

typedef struct {
    uint32_t magic;
    uint32_t kind;
    uint32_t pc, lr, psr, cfsr, hfsr, mmfar, bfar, detail;
    char where[24];
    uint32_t boots;        /* resets observed while the record stayed valid */
    uint32_t check;
} crash_record_t;

/* Called once from Reset_Handler: validates or clears the record, keeps the
 * previous record available to crash_log_previous(). */
void crash_log_boot(void);
/* Copy of the record that explains the current boot (kind CRASH_NONE if the
 * record was absent/corrupt, e.g. after a power-on). */
const crash_record_t *crash_log_previous(void);
uint32_t crash_log_checksum(const crash_record_t *record);
/* Weak hook called with the record just before the reset (default: none). */
void crash_log_emit(const crash_record_t *record);

/* Each recorder stores the record and resets the MCU. */
_Noreturn void crash_log_record_irq(uint32_t exception);
_Noreturn void crash_log_record_fault(const uint32_t *frame, uint32_t exc_return);
_Noreturn void crash_log_record_text(uint32_t kind, const char *where, uint32_t detail);
#endif
