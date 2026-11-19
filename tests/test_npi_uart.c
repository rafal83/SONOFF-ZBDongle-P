/* Compile the actual generated TI UART function bodies against a driver model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void *UART2_Handle;
typedef uint16_t uint16;
typedef void (*npiCB_t)(uint16_t, uint16_t);
typedef struct {
    uint32_t baudRate, eventMask;
    int dataLength, stopBits, readMode, writeMode, readReturnMode;
    void (*readCallback)(UART2_Handle, void *, size_t, void *, int_fast16_t);
    void (*writeCallback)(UART2_Handle, void *, size_t, void *, int_fast16_t);
    void (*eventCallback)(UART2_Handle, uint32_t, uint32_t, void *);
} UART2_Params;
enum { UART2_DataLen_8, UART2_StopBits_1, UART2_Mode_CALLBACK, UART2_ReadReturnMode_PARTIAL };
#define UART2_STATUS_SUCCESS 0
#define UART2_EVENT_FRAMING 1
#define UART2_EVENT_PARITY 2
#define UART2_EVENT_BREAK 4
#define UART2_EVENT_OVERRUN 8
#define UART2_EVENT_TX_FINISHED 32
#define NPI_FLOW_CTRL 0
#define CONFIG_DISPLAY_UART 0
#define NPI_UART_BR 115200
#define NPI_TL_BUF_SIZE 256
#define UART_ISR_BUF_SIZE 128
static UART2_Handle uartHandle;
static uint32_t port_uart_rate;
static volatile bool port_uart_reconfiguring;
static uint8_t isrRxBuf[UART_ISR_BUF_SIZE], *TransportRxBuf, *TransportTxBuf;
static uint16_t TransportRxLen, TransportTxLen;
static npiCB_t npiTransmitCB;
const uint32_t port_npi_baudrate = NPI_UART_BR;
static UART2_Params configured;
static unsigned reads, opens, closes, cancels, errors, starts, deliveries, critical;
static uint32_t fail_rate;
static bool reject_write, fail_all_opens;
uint32_t OsalPort_enterCS(void) { return critical++; }
void OsalPort_leaveCS(uint32_t key) { critical = key; }
static void NPITLUART_readCallBack(UART2_Handle, void *, size_t, void *, int_fast16_t);
static void NPITLUART_eventCallBack(UART2_Handle, uint32_t, uint32_t, void *);
static void NPITLUART_writeCallBack(UART2_Handle h, void *p, size_t n, void *a, int_fast16_t s) {
    (void)h; (void)p; (void)n; (void)a; (void)s;
}
void PortAutobaud_start(void) { starts++; }
void PortAutobaud_error(void) { errors++; }
void UART2_Params_init(UART2_Params *p) { memset(p, 0, sizeof(*p)); }
UART2_Handle UART2_open(unsigned index, UART2_Params *p) {
    assert(!index); opens++; configured = *p;
    return fail_all_opens || p->baudRate == fail_rate ? NULL : (void *)1;
}
void UART2_close(UART2_Handle h) { assert(h); closes++; }
void UART2_readCancel(UART2_Handle h) {
    assert(h); cancels++;
    unsigned previous_reads = reads, previous_deliveries = deliveries;
    configured.readCallback(h, isrRxBuf, 2, NULL, -1);
    assert(reads == previous_reads && deliveries == previous_deliveries);
}
int UART2_read(UART2_Handle h, void *p, size_t n, void *out) {
    (void)p; (void)out; assert(h && n == UART_ISR_BUF_SIZE); reads++; return 0;
}
int UART2_write(UART2_Handle h, void *p, size_t n, void *out) {
    (void)p; (void)n; (void)out; assert(h); return reject_write ? -1 : 0;
}
void NPITLUART_readTransport(void) {
    TransportRxLen = 0; UART2_read(uartHandle, isrRxBuf, UART_ISR_BUF_SIZE, NULL);
}
static uint16_t NPITLUART_readIsrBuf(size_t n) { TransportRxLen += (uint16_t)n; return (uint16_t)n; }
static void receive(uint16_t rx, uint16_t tx) { (void)rx; (void)tx; deliveries++; }
#include "npi_uart_under_test.inc"
int main(void) {
    uint8_t rx[256], tx[256];
    assert(!NPITLUART_writeTransport(5));
    NPITLUART_initializeTransport(rx, tx, receive);
    assert(uartHandle && opens == 1 && reads == 1 && starts == 1);
    assert(configured.baudRate == 115200 && configured.eventMask == 47);
    assert(NPITLUART_setBaud(460800) && NPITLUART_getBaud() == 460800);
    assert(closes == 1 && cancels == 1 && reads == 2 && !deliveries && !errors);
    assert(!NPITLUART_setBaud(9600));
    fail_rate = 1000000;
    assert(!NPITLUART_setBaud(1000000) && NPITLUART_getBaud() == 460800 && uartHandle);
    assert(cancels == 2 && closes == 2 && opens == 4);
    fail_rate = 0; assert(NPITLUART_setBaud(1000000));
    configured.readCallback(uartHandle, isrRxBuf, 5, NULL, 0);
    assert(deliveries == 1 && TransportRxLen == 0);
    configured.readCallback(uartHandle, isrRxBuf, 5, NULL, -1);
    assert(errors == 1 && TransportRxLen == 0);
    configured.eventCallback(uartHandle, UART2_EVENT_PARITY | UART2_EVENT_TX_FINISHED, 0, NULL);
    assert(errors == 2 && deliveries == 3);
    assert(NPITLUART_writeTransport(5) == 5);
    reject_write = true; assert(NPITLUART_writeTransport(5) == 0);
    fail_all_opens = true;
    assert(!NPITLUART_setBaud(115200) && !NPITLUART_getBaud() && !uartHandle);
    assert(!NPITLUART_writeTransport(5));
    fail_all_opens = false;
    NPITLUART_initializeTransport(rx, tx, receive);
    assert(NPITLUART_getBaud() == 1000000 && uartHandle);
    assert(!critical && !port_uart_reconfiguring);
    puts("PASS: actual TI UART overlay, callback-safe cancel/reopen, 3 baudrates, failed-open restoration, error events and TX rejection");
}
