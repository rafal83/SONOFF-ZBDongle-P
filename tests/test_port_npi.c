/* Executes the production TI adapter with mocked clock/UART/allocator APIs. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "npi.h"
#include "port_npi.h"
#include <ti/sysbios/knl/Semaphore.h>
extern npi_tx_queue port_npi_tx;
extern npi_parser port_npi_parser;
extern uint32_t port_npi_rx_no_memory, port_npi_tx_retries;
Semaphore_Handle npiSemHandle = (void *)1;
uint32_t port_radio_rejected = 7, port_radio_last_rejection = 0x02080721;
static uint64_t ticks;
static unsigned cs, posts, allocated, delivered, writes;
static bool fail_alloc, busy, reject_write;
static uint8_t rx[4096], transmitted[255];
static unsigned rx_head, rx_tail;
static uint16_t transmitted_size;
bool PortAutobaud_poll(uint32_t now) { (void)now; return false; }
void PortAutobaud_valid(void) {}
uint32_t PortAutobaud_waitMs(void) { return UINT32_MAX; }
uint64_t ClockP_getSystemTicks64(void) { return ticks; }
uint32_t ClockP_getSystemTickPeriod(void) { return 10; }
uint32_t OsalPort_enterCS(void) { return cs++; }
void OsalPort_leaveCS(uint32_t key) { cs = key; }
void *OsalPort_msgAllocate(uint16_t size) {
    if (fail_alloc) return NULL;
    void *p = malloc(size); assert(p); allocated++; return p;
}
void OsalPort_msgDeallocate(void *p) { assert(allocated); allocated--; free(p); }
void Semaphore_post(Semaphore_Handle h) { assert(h == npiSemHandle); posts++; }
uint16_t NPIRxBuf_GetRxBufCount(void) { return (uint16_t)(rx_tail - rx_head); }
uint16_t NPIRxBuf_ReadFromRxBuf(uint8_t *p, uint16_t n) {
    assert(n == 1 && rx_head < rx_tail); *p = rx[rx_head++]; return 1;
}
uint8_t NPITL_checkNpiBusy(void) { return busy; }
uint16_t NPITL_writeTL(uint8_t *p, uint16_t n) {
    assert(cs && n <= sizeof(transmitted) && !busy);
    if (reject_write) return 0;
    memcpy(transmitted, p, n); transmitted_size = n; writes++; busy = true; return n;
}
static void receive(uint8_t size, uint8_t *p, NPIMSG_Type type) {
    assert(type == NPIMSG_Type_ASYNC && size == 3 && p[0] == 0 && p[1] == 0x21 && p[2] == 1);
    delivered++; OsalPort_msgDeallocate(p);
}
static void feed(const uint8_t *p, size_t n) {
    assert(n <= sizeof(rx)); rx_head = 0; rx_tail = (unsigned)n; memcpy(rx, p, n); PortNPI_collect();
}
static void send(uint8_t cmd0, uint8_t cmd1, uint8_t value) {
    uint8_t *p = OsalPort_msgAllocate(4); p[0] = 1; p[1] = cmd0; p[2] = cmd1; p[3] = value;
    PortNPI_sendToHost(p);
}
int main(void) {
    const uint8_t ping[] = {0xfe, 0, 0x21, 1, 0x20};
    PortNPI_initialize(receive);
    assert(PortNPI_waitTicks() == UINT32_MAX);
    const uint8_t undecodable[] = {0x19, 0x80};
    feed(undecodable, sizeof(undecodable)); assert(!delivered);
    uint8_t noise[305]; memset(noise, 0xfe, 300); memcpy(noise + 300, ping, 5);
    feed(noise, sizeof(noise)); assert(delivered == 1 && !allocated);
    feed(ping, 4); feed(ping, 5); /* Missing FCS: keep valid suffix until deadline. */
    ticks += 10000; PortNPI_collect(); assert(delivered == 2 && !allocated);
    const uint8_t incomplete[] = {0xfe, 250, 0x21};
    feed(incomplete, sizeof(incomplete)); assert(PortNPI_waitTicks() == 10000);
    ticks += 10000; PortNPI_collect(); assert(!port_npi_parser.count);
    fail_alloc = true; feed(ping, 5); fail_alloc = false;
    assert(port_npi_rx_no_memory == 1 && !allocated);
    feed(ping, 5); assert(delivered == 3);
    for (unsigned i = 0; i < 6; i++) send(0x44, 0x81, (uint8_t)i);
    send(0x44, 0x81, 9); assert(port_npi_tx.dropped_normal == 1 && !allocated);
    send(0x61, 1, 0xaa); send(0x44, 0x80, 0xbb);
    assert(port_npi_tx.count == 8 && PortNPI_waitTicks() == 1000);
    reject_write = true; PortNPI_pumpTx(); reject_write = false;
    assert(port_npi_tx.count == 8 && port_npi_tx_retries == 1);
    PortNPI_pumpTx(); assert(transmitted_size == 6 && transmitted[2] == 0x61 && transmitted[4] == 0xaa);
    assert(port_npi_tx.count == 7); PortNPI_pumpTx(); assert(writes == 1); /* busy transport owns full frame */
    busy = false; PortNPI_pumpTx(); assert(transmitted[2] == 0x44 && transmitted[3] == 0x80);
    while (port_npi_tx.count) { busy = false; PortNPI_pumpTx(); }
    busy = false;
    assert(!allocated && !cs && posts >= 8 && PortNPI_waitTicks() == UINT32_MAX);
    /* TI ticks wrap at 2^32; millisecond deadlines must continue using tick64. */
    ticks = UINT32_MAX - 30u; feed(incomplete, sizeof(incomplete));
    ticks += 10000; PortNPI_collect(); assert(!port_npi_parser.count);
    const uint8_t diagnostic[] = {0xfe, 0, 0x21, 0xf0, 0xd1};
    unsigned old_delivered = delivered;
    feed(diagnostic, sizeof(diagnostic));
    assert(delivered == old_delivered && !allocated && port_npi_tx.count == 1);
    busy = false; PortNPI_pumpTx();
    assert(transmitted_size == 37 && transmitted[2] == 0x61 && transmitted[3] == 0xf0);
    uint32_t snapshot[8]; memcpy(snapshot, transmitted + 4, sizeof(snapshot));
    assert(snapshot[0] == 1 && snapshot[1] == 7 && snapshot[2] == 0x02080721);
    uint8_t checksum = 0;
    for (unsigned i = 1; i < transmitted_size; i++) checksum ^= transmitted[i];
    assert(!checksum && !cs);
    puts("PASS: production NPI adapter, RX noise/truncation/deadlines/tick wrap/allocation failure, fixed TX pool/reserve/priority/driver rejection/busy ownership and read-only diagnostic snapshot");
    return 0;
}
