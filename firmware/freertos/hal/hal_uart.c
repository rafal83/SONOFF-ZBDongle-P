/* SPDX-License-Identifier: MIT
 * UART0 driver for the host link (CP2102N). Original code on TI driverlib.
 */
#include "hal_uart.h"
#include "hal_sys.h"
#include "board.h"
#include "spsc_ring.h"

#include <inc/hw_memmap.h>
#include <inc/hw_ints.h>
#include <inc/hw_types.h>
#include <inc/hw_uart.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/uart.h>
#include <inc/hw_prcm.h>

static uint8_t rx_storage[HAL_UART_RX_RING];
static uint8_t tx_storage[HAL_UART_TX_RING];
static spsc_ring_t rx_ring, tx_ring;
static hal_uart_stats_t stats;
/* First unconsumed RX error: its ring position and the accumulated classes.
 * The consumer stops reading at that position, aborts the parser frame, then
 * continues; later errors before consumption merge into the same abort (the
 * dropped bytes also make the frame FCS fail). */
static volatile uint32_t rx_error_mask, rx_error_pos;
static void (*notify)(void);

void hal_uart_init(uint32_t baud, void (*notify_from_isr)(void)) {
    spsc_ring_init(&rx_ring, rx_storage, sizeof rx_storage);
    spsc_ring_init(&tx_ring, tx_storage, sizeof tx_storage);
    notify = notify_from_isr;
    UARTDisable(UART0_BASE);
    IOCPortConfigureSet(BOARD_UART_TX_DIO, IOC_PORT_MCU_UART0_TX, IOC_STD_OUTPUT);
    IOCPortConfigureSet(BOARD_UART_RX_DIO, IOC_PORT_MCU_UART0_RX, IOC_STD_INPUT);
    UARTConfigSetExpClk(UART0_BASE, 48000000u, baud,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTFIFOLevelSet(UART0_BASE, UART_FIFO_TX1_8, UART_FIFO_RX4_8);
    UARTFIFOEnable(UART0_BASE);
    UARTIntClear(UART0_BASE, 0xFFFu);
    UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT | UART_INT_OE | UART_INT_BE |
                              UART_INT_PE | UART_INT_FE);
    IntPrioritySet(INT_UART0_COMB, BOARD_IRQ_PRIO_UART);
    IntPendClear(INT_UART0_COMB);
    IntEnable(INT_UART0_COMB);
    UARTEnable(UART0_BASE);
}

/* Fill the hardware FIFO from the TX ring. Caller: ISR, or task with the
 * UART interrupt masked. Disables the TX interrupt once the ring is empty. */
static void tx_pump(void) {
    uint8_t byte;
    while (!(HWREG(UART0_BASE + UART_O_FR) & UART_FR_TXFF)) {
        if (!spsc_ring_get(&tx_ring, &byte)) {
            HWREG(UART0_BASE + UART_O_IMSC) &= ~UART_INT_TX;
            return;
        }
        HWREG(UART0_BASE + UART_O_DR) = byte;
        stats.tx_bytes++;
    }
    HWREG(UART0_BASE + UART_O_IMSC) |= UART_INT_TX;
}

void UART0_IRQHandler(void) {
    uint32_t start = hal_cycles();
    uint32_t status = HWREG(UART0_BASE + UART_O_MIS);
    HWREG(UART0_BASE + UART_O_ICR) = status & ~UART_INT_TX;
    bool wake = false;
    while (!(HWREG(UART0_BASE + UART_O_FR) & UART_FR_RXFE)) {
        uint32_t word = HWREG(UART0_BASE + UART_O_DR);
        if (word & 0xF00u) {
            if (word & UART_DR_OE) stats.hw_overrun++;
            if (word & UART_DR_BE) stats.brk++;
            if (word & UART_DR_PE) stats.parity++;
            if (word & UART_DR_FE) stats.framing++;
            if (!rx_error_mask) rx_error_pos = rx_ring.head;
            rx_error_mask |= (word >> 8) & 0xFu;
            HWREG(UART0_BASE + UART_O_ECR) = 0;
            wake = true;
            continue;
        }
        if (spsc_ring_put(&rx_ring, (uint8_t)word)) stats.rx_bytes++;
        else stats.rx_overflow++;
        wake = true;
    }
    uint32_t used = spsc_ring_used(&rx_ring);
    if (used > stats.rx_high_water) stats.rx_high_water = (uint16_t)used;
    if (status & UART_INT_TX) {
        tx_pump();
        if (spsc_ring_free(&tx_ring) >= HAL_UART_TX_RING / 2) wake = true;
    }
    stats.isr_count++;
    uint32_t spent = hal_cycles() - start;
    if (spent > stats.isr_max_cycles) stats.isr_max_cycles = spent;
    if (wake && notify) notify();
}

size_t hal_uart_read(uint8_t *out, size_t max) {
    /* Snapshot the readable limit atomically with respect to the ISR so no
     * byte received after a not-yet-seen error can slip through. */
    IntDisable(INT_UART0_COMB);
    uint32_t limit = (rx_error_mask ? rx_error_pos : rx_ring.head) - rx_ring.tail;
    IntEnable(INT_UART0_COMB);
    if (limit < max) max = limit;
    return spsc_ring_read(&rx_ring, out, max);
}

size_t hal_uart_tx_free(void) { return spsc_ring_free(&tx_ring); }

size_t hal_uart_write(const uint8_t *data, size_t len) {
    size_t n = spsc_ring_write(&tx_ring, data, len);
    uint32_t used = spsc_ring_used(&tx_ring);
    if (used > stats.tx_high_water) stats.tx_high_water = (uint16_t)used;
    /* Prime the FIFO with the UART interrupt masked: the ISR is the normal
     * consumer, this is the only other one and they never run concurrently. */
    IntDisable(INT_UART0_COMB);
    tx_pump();
    IntEnable(INT_UART0_COMB);
    return n;
}

uint32_t hal_uart_take_rx_errors(void) {
    IntDisable(INT_UART0_COMB);
    uint32_t mask = 0;
    if (rx_error_mask && rx_error_pos == rx_ring.tail) {
        mask = rx_error_mask;
        rx_error_mask = 0;
    }
    IntEnable(INT_UART0_COMB);
    return mask;
}

void hal_uart_stats(hal_uart_stats_t *out) {
    IntDisable(INT_UART0_COMB);
    *out = stats;
    IntEnable(INT_UART0_COMB);
}

bool hal_uart_tx_idle(void) {
    return spsc_ring_used(&tx_ring) == 0 && !(HWREG(UART0_BASE + UART_O_FR) & UART_FR_BUSY);
}

static void configure_port(uint32_t baud) {
    UARTDisable(UART0_BASE);
    IOCPortConfigureSet(BOARD_UART_TX_DIO, IOC_PORT_MCU_UART0_TX, IOC_STD_OUTPUT);
    IOCPortConfigureSet(BOARD_UART_RX_DIO, IOC_PORT_MCU_UART0_RX, IOC_STD_INPUT);
    UARTConfigSetExpClk(UART0_BASE, 48000000u, baud,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTFIFOEnable(UART0_BASE);
    UARTEnable(UART0_BASE);
}

void hal_uart_early_init(uint32_t baud) { configure_port(baud); }

static bool uart_usable(void) {
    return (HWREG(PRCM_BASE + PRCM_O_PDSTAT0) & PRCM_PDSTAT0_SERIAL_ON) &&
           (HWREG(PRCM_BASE + PRCM_O_UARTCLKGR) & PRCM_UARTCLKGR_CLK_EN_M) &&
           (HWREG(UART0_BASE + UART_O_CTL) & UART_CTL_UARTEN);
}

static void polled_byte(uint8_t b) {
    for (uint32_t i = 0; i < 200000u && (HWREG(UART0_BASE + UART_O_FR) & UART_FR_TXFF); i++) { }
    HWREG(UART0_BASE + UART_O_DR) = b;
}

void hal_uart_polled_frame(uint8_t cmd1, const uint8_t *payload, uint8_t length) {
    if (!uart_usable()) return;
    uint8_t fcs = (uint8_t)(length ^ 0x5Fu ^ cmd1);
    polled_byte(0xFE);
    polled_byte(length);
    polled_byte(0x5F);
    polled_byte(cmd1);
    for (uint8_t i = 0; i < length; i++) { polled_byte(payload[i]); fcs ^= payload[i]; }
    polled_byte(fcs);
    for (uint32_t i = 0; i < 400000u && (HWREG(UART0_BASE + UART_O_FR) & UART_FR_BUSY); i++) { }
}
