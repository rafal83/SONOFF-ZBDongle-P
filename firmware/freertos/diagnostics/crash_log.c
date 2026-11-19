/* SPDX-License-Identifier: MIT */
#include "crash_log.h"

#define CRASH_MAGIC 0x46525443u /* "FRTC" */
#define SCB_CFSR  (*(volatile uint32_t *)0xE000ED28u)
#define SCB_HFSR  (*(volatile uint32_t *)0xE000ED2Cu)
#define SCB_MMFAR (*(volatile uint32_t *)0xE000ED34u)
#define SCB_BFAR  (*(volatile uint32_t *)0xE000ED38u)
#define SCB_AIRCR (*(volatile uint32_t *)0xE000ED0Cu)

__attribute__((section(".noinit"))) static crash_record_t live;
static crash_record_t previous;

uint32_t crash_log_checksum(const crash_record_t *r) {
    /* FNV-1a over every byte before `check`: detects random power-on SRAM. */
    const uint8_t *p = (const uint8_t *)r;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < (unsigned)__builtin_offsetof(crash_record_t, check); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void seal(void) {
    live.magic = CRASH_MAGIC;
    live.check = crash_log_checksum(&live);
}

void crash_log_boot(void) {
    bool valid = live.magic == CRASH_MAGIC && live.check == crash_log_checksum(&live);
    if (valid) {
        previous = live;
        live.boots++;
    } else {
        previous = (crash_record_t){0};
        live = (crash_record_t){0};
    }
    /* Keep the boot counter, forget the reason: a later clean reset must not
     * re-report an old crash. */
    live.kind = CRASH_NONE;
    seal();
}

const crash_record_t *crash_log_previous(void) { return &previous; }

__attribute__((weak)) void crash_log_emit(const crash_record_t *record) { (void)record; }

static _Noreturn void reset_now(void) {
    seal();
    crash_log_emit(&live);
    __asm volatile("dsb" ::: "memory");
    SCB_AIRCR = 0x05FA0004u; /* VECTKEY | SYSRESETREQ */
    __asm volatile("dsb" ::: "memory");
    for (;;) { }
}

static void copy_where(const char *where) {
    unsigned n = 0;
    if (where) {
        /* Keep the tail of long paths: the file name is the useful part. */
        unsigned len = 0;
        while (where[len]) len++;
        const char *start = len >= sizeof live.where ? where + len - (sizeof live.where - 1) : where;
        while (start[n] && n < sizeof live.where - 1) { live.where[n] = start[n]; n++; }
    }
    while (n < sizeof live.where) live.where[n++] = 0;
}

_Noreturn void crash_log_record_irq(uint32_t exception) {
    __asm volatile("cpsid i" ::: "memory");
    live.kind = CRASH_UNEXPECTED_IRQ;
    live.detail = exception;
    live.cfsr = SCB_CFSR;
    live.hfsr = SCB_HFSR;
    copy_where("irq");
    reset_now();
}

_Noreturn void crash_log_record_fault(const uint32_t *frame, uint32_t exc_return) {
    __asm volatile("cpsid i" ::: "memory");
    live.kind = CRASH_HARD_FAULT;
    live.lr = frame[5];
    live.pc = frame[6];
    live.psr = frame[7];
    live.cfsr = SCB_CFSR;
    live.hfsr = SCB_HFSR;
    live.mmfar = SCB_MMFAR;
    live.bfar = SCB_BFAR;
    live.detail = exc_return;
    copy_where("fault");
    reset_now();
}

_Noreturn void crash_log_record_text(uint32_t kind, const char *where, uint32_t detail) {
    __asm volatile("cpsid i" ::: "memory");
    live.kind = kind;
    live.detail = detail;
    uint32_t lr;
    __asm volatile("mov %0, lr" : "=r"(lr));
    live.lr = lr;
    copy_where(where);
    reset_now();
}
