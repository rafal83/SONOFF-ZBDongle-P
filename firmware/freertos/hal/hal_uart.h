/* SPDX-License-Identifier: MIT
 * Interrupt-driven UART0 with fixed SPSC rings. The ISR is the only RX
 * producer and TX consumer; one task is the only RX consumer and TX producer.
 */
#ifndef FRT_HAL_UART_H
#define FRT_HAL_UART_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAL_UART_RX_RING 1024u   /* power of two; ~10 ms of input at 1 Mbaud */
#define HAL_UART_TX_RING 1024u

typedef struct {
    uint32_t rx_bytes, tx_bytes;
    uint32_t rx_overflow;        /* bytes lost because the ring was full */
    uint32_t hw_overrun, framing, parity, brk;
    uint32_t isr_count, isr_max_cycles;
    uint16_t rx_high_water, tx_high_water;
} hal_uart_stats_t;

/* notify(context) is called from the ISR after RX data or TX space. */
void hal_uart_init(uint32_t baud, void (*notify_from_isr)(void));
size_t hal_uart_read(uint8_t *out, size_t max);
/* Copies up to len bytes; returns the number accepted (never blocks). */
size_t hal_uart_write(const uint8_t *data, size_t len);
size_t hal_uart_tx_free(void);
/* hal_uart_read never returns bytes received after a pending RX error.
 * Once the consumer reached it, this returns the error classes (OE/BE/PE/FE
 * bits 3..0) and clears them; otherwise 0. */
uint32_t hal_uart_take_rx_errors(void);
void hal_uart_stats(hal_uart_stats_t *out);
/* Polled output, no interrupt, no RTOS: boot milestones and crash records.
 * hal_uart_early_init() may run before the scheduler; hal_uart_polled_frame()
 * is safe from fault handlers: it checks power domain, clock gate and enable
 * bit first, and every wait is bounded. Frames are MT AREQ 0x5F <cmd1>. */
void hal_uart_early_init(uint32_t baud);
void hal_uart_polled_frame(uint8_t cmd1, const uint8_t *payload, uint8_t length);
/* True when the TX ring is empty and the shift register finished. */
bool hal_uart_tx_idle(void);
#endif
