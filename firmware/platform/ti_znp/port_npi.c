/* SPDX-License-Identifier: MIT
 * Adapter for the documented TI MT/NPI APIs.
 */
#include "port_npi.h"
#include "port_autobaud.h"
#include "radio_guard.h"
#include "npi.h"
#include "npi_rxbuf.h"
#include "npi_tl.h"
#include "osal_port.h"
#include <ti/drivers/dpl/ClockP.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <string.h>
#ifdef NPI_SREQRSP
#error "This adapter requires the standard ZNP configuration without NPI_SREQRSP"
#endif
extern Semaphore_Handle npiSemHandle;
#ifndef NPI_UART_BR
#define NPI_UART_BR 115200
#endif
const uint32_t port_npi_baudrate = NPI_UART_BR;
#ifndef PORT_AUTOBAUD_ENABLE
#define PORT_AUTOBAUD_ENABLE 0
#endif
const uint32_t port_autobaud_enabled = PORT_AUTOBAUD_ENABLE;
static npiIncomingFrameCBack_t incoming;
/* Parser: NPI task only. TX pool: every access serialized by the TI CS. */
npi_parser port_npi_parser;
npi_tx_queue port_npi_tx;
uint32_t port_npi_rx_no_memory, port_npi_tx_retries;
static uint32_t now_ms(void) {
    return (uint32_t)((ClockP_getSystemTicks64() * ClockP_getSystemTickPeriod()) / 1000u);
}
static void deliver(void *context, const npi_frame *frame) {
    (void)context;
    PortAutobaud_valid();
    /* Read-only laboratory extension. Never expose RAM, payloads or NV.
     * Standard MT commands keep their existing handlers and semantics. */
    if (frame->cmd0 == 0x21 && frame->cmd1 == 0xf0 && !frame->length) {
        uint32_t key = OsalPort_enterCS();
        uint32_t stats[8] = {1, port_radio_rejected, port_radio_last_rejection,
            port_npi_rx_no_memory, port_npi_tx_retries, port_npi_tx.dropped_normal,
            port_npi_tx.dropped_control, port_npi_parser.rx_errors};
        bool queued = npi_tx_enqueue_fields(&port_npi_tx, 0x61, 0xf0,
            (const uint8_t *)stats, sizeof(stats), NPI_TX_CONTROL);
        OsalPort_leaveCS(key);
        if (queued) Semaphore_post(npiSemHandle);
        return;
    }
    if (!incoming) return;
    uint8_t size = (uint8_t)(frame->length + 3u);
    uint8_t *message = OsalPort_msgAllocate(size);
    if (!message) { port_npi_rx_no_memory++; return; }
    message[0] = frame->length; message[1] = frame->cmd0; message[2] = frame->cmd1;
    memcpy(message + 3, frame->payload, frame->length);
    incoming(size, message, NPIMSG_Type_ASYNC);
}
void PortNPI_initialize(npiIncomingFrameCBack_t receive) {
    incoming = receive;
    npi_init(&port_npi_parser, deliver, NULL);
    npi_tx_init(&port_npi_tx);
}
void PortNPI_collect(void) {
    uint32_t now = now_ms();
    npi_expire(&port_npi_parser, now);
    while (NPIRxBuf_GetRxBufCount()) {
        uint8_t byte;
        NPIRxBuf_ReadFromRxBuf(&byte, 1);
        npi_feed(&port_npi_parser, byte, now);
    }
    /* Give an already valid UART frame priority over a timing proposal.
     * Re-arm before transmitting its reply so a fast host can change speed
     * immediately after receiving that reply. */
    if (PortAutobaud_poll(now)) npi_rx_error(&port_npi_parser);
}
uint32_t PortNPI_waitTicks(void) {
    uint32_t ms = PortAutobaud_waitMs();
    if (port_npi_parser.count) {
        uint32_t now = now_ms();
        uint32_t idle = now - port_npi_parser.last_ms, age = now - port_npi_parser.started_ms;
        uint32_t idle_remaining = idle >= NPI_TIMEOUT_MS ? 0 : NPI_TIMEOUT_MS - idle;
        if (idle_remaining < ms) ms = idle_remaining;
        uint32_t remaining = age >= NPI_FRAME_TIMEOUT_MS ? 0 : NPI_FRAME_TIMEOUT_MS - age;
        if (remaining < ms) ms = remaining;
    }
    uint32_t key = OsalPort_enterCS();
    if (port_npi_tx.count && !NPITL_checkNpiBusy() && ms > 10) ms = 10;
    OsalPort_leaveCS(key);
    if (ms == UINT32_MAX) return UINT32_MAX;
    uint32_t period = ClockP_getSystemTickPeriod();
    uint32_t ticks = (ms * 1000u + period - 1u) / period;
    return ticks ? ticks : 1;
}
void PortNPI_sendToHost(uint8_t *message) {
    if (!message) return;
    uint8_t cmd0 = message[1], cmd1 = message[2];
    bool control = (cmd0 & 0xe0u) == 0x60u || ((cmd0 == 0x44u || cmd0 == 0x41u) && cmd1 == 0x80u);
    uint32_t key = OsalPort_enterCS();
    bool queued = npi_tx_enqueue_fields(&port_npi_tx, cmd0, cmd1, message + 3, message[0],
                                        control ? NPI_TX_CONTROL : NPI_TX_NORMAL);
    OsalPort_leaveCS(key);
    /* Exactly the same ownership transfer as TI's NPIFrame_frameMsg. */
    OsalPort_msgDeallocate(message);
    if (queued) Semaphore_post(npiSemHandle);
}
void PortNPI_pumpTx(void) {
    uint32_t key = OsalPort_enterCS();
    if (!NPITL_checkNpiBusy()) {
        const uint8_t *wire;
        uint16_t size = (uint16_t)npi_tx_frame(&port_npi_tx, &wire);
        if (size) {
            /* TI TL copies a <=255 byte frame synchronously before returning.
             * Keep the slot until that copy is accepted; never interleave frames.
             */
            if (NPITL_writeTL((uint8_t *)wire, size) == size) npi_tx_finish_frame(&port_npi_tx);
            else port_npi_tx_retries++;
        }
    }
    OsalPort_leaveCS(key);
}
