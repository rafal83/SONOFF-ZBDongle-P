/* SPDX-License-Identifier: MIT
 * Cortex-M4F reset and vector table for the CC2652P FreeRTOS target.
 * Original code; only the TI driverlib SetupTrimDevice() (BSD) is called.
 */
#include <stdint.h>
#include "crash_log.h"

#include <inc/hw_types.h>
#include <driverlib/setup.h>

extern uint32_t _data, _edata, _ldata, _bss, _ebss, _estack;
int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void NMI_Handler(void);
void HardFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void UART0_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void RFC_CPE0_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void RFC_CPE1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void RFC_HW_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void RFC_CMD_ACK_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void WDT_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void CRYPTO_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TRNG_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

typedef void (*vector_t)(void);

/* Stable identity for restoring an application without rolling network data back.
 * Increment storage_layout before any incompatible nv_log format change. */
__attribute__((section(".imageInfo"), used))
const struct {
    char magic[8];
    uint32_t version, storage_layout, uart_baud, revision;
} image_info = { "ZBDP-FRT", 1u, 2u, BOARD_UART_BAUD, FRT_BUILD_REVISION };   /* layout 2: 2 x 56 KiB store */

/* 16 system exceptions + 38 device interrupts (hw_ints.h: 16..53). */
__attribute__((section(".resetVecs"), used))
const vector_t vector_table[54] = {
    (vector_t)(uintptr_t)&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    HardFault_Handler,      /* MemManage: escalated, same record */
    HardFault_Handler,      /* BusFault */
    HardFault_Handler,      /* UsageFault */
    0, 0, 0, 0,
    SVC_Handler,
    Default_Handler,        /* DebugMon */
    0,
    PendSV_Handler,
    SysTick_Handler,
    Default_Handler,        /* 16 AON GPIO edge */
    Default_Handler,        /* 17 I2C */
    RFC_CPE1_IRQHandler,    /* 18 RF CPE1 */
    Default_Handler,        /* 19 PKA */
    Default_Handler,        /* 20 AON RTC */
    UART0_IRQHandler,       /* 21 UART0 */
    Default_Handler,        /* 22 AUX SWEV0 */
    Default_Handler,        /* 23 SSI0 */
    Default_Handler,        /* 24 SSI1 */
    RFC_CPE0_IRQHandler,    /* 25 RF CPE0 */
    RFC_HW_IRQHandler,      /* 26 RF HW */
    RFC_CMD_ACK_IRQHandler, /* 27 RF doorbell ACK */
    Default_Handler,        /* 28 I2S */
    Default_Handler,        /* 29 AUX SWEV1 */
    WDT_IRQHandler,         /* 30 Watchdog */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 31-34 GPT */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* 35-38 GPT */
    CRYPTO_IRQHandler,      /* 39 Crypto */
    Default_Handler,        /* 40 DMA done */
    Default_Handler,        /* 41 DMA error */
    Default_Handler,        /* 42 Flash */
    Default_Handler,        /* 43 SW event 0 */
    Default_Handler,        /* 44 AUX combined */
    Default_Handler,        /* 45 AON prog 0 */
    Default_Handler,        /* 46 prog 0 */
    Default_Handler,        /* 47 AUX COMPA */
    Default_Handler,        /* 48 AUX ADC */
    TRNG_IRQHandler,        /* 49 TRNG */
    Default_Handler,        /* 50 OSC */
    Default_Handler,        /* 51 AUX timer2 */
    Default_Handler,        /* 52 UART1 */
    Default_Handler,        /* 53 BATMON */
};

void Reset_Handler(void) {
    /* Mandatory device trims from FCFG1 (TI driverlib, must run first). */
    SetupTrimDevice();
    uint32_t *src = &_ldata;
    for (uint32_t *dst = &_data; dst < &_edata;) *dst++ = *src++;
    for (uint32_t *dst = &_bss; dst < &_ebss;) *dst++ = 0;
    /* Enable CP10/CP11 (FPU) before any FP instruction or lazy stacking. */
    HWREG(0xE000ED88u) |= (0xFu << 20);
    __asm volatile("dsb\n isb" ::: "memory");
    crash_log_boot();
    (void)main();
    for (;;) { }
}

void Default_Handler(void) {
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    crash_log_record_irq(ipsr & 0x1FFu);
}

void NMI_Handler(void) {
    crash_log_record_irq(2u);
}

/* Pick the stack that was active when the fault happened and hand the
 * exception frame to C. Naked: no prologue may touch the faulting stack. */
__attribute__((naked)) void HardFault_Handler(void) {
    __asm volatile(
        "tst lr, #4      \n"
        "ite eq          \n"
        "mrseq r0, msp   \n"
        "mrsne r0, psp   \n"
        "mov r1, lr      \n"
        "b crash_log_record_fault \n");
}
