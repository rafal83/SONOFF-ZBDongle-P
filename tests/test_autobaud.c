/* Tests the production acquisition controller; the capture peripheral/UART
 * are simulated, not a substitute for timing measurements on a CC2652P. */
#include <assert.h>
#ifndef PORT_AUTOBAUD_ENABLE
#define PORT_AUTOBAUD_ENABLE 1
#endif
const unsigned int port_autobaud_enabled = PORT_AUTOBAUD_ENABLE;
#include <stdio.h>
#include <string.h>
#include "autobaud.h"
#include "port_autobaud.h"
#include <ti/sysbios/knl/Semaphore.h>
Semaphore_Handle npiSemHandle = (void *)1;
static bool capture, complete, busy, available = true, fail_open;
static uint32_t edge[4], rate = 115200;
static unsigned changed, stopped, posted, critical;
uint32_t OsalPort_enterCS(void) { return critical++; }
void OsalPort_leaveCS(uint32_t key) { critical = key; }
void Semaphore_post(Semaphore_Handle h) { assert(h == npiSemHandle); posted++; }
uint8_t NPITL_checkNpiBusy(void) { return busy; }
bool PortBaudCapture_start(void) { if (!capture && available) capture = true; return capture; }
void PortBaudCapture_stop(void) { assert(capture); capture = complete = false; stopped++; }
bool PortBaudCapture_read(uint32_t out[4]) { assert(capture); memcpy(out, edge, sizeof(edge)); return complete; }
uint32_t PortBaudCapture_clock(void) { return 48000000; }
uint32_t NPITLUART_getBaud(void) { return rate; }
bool NPITLUART_setBaud(uint32_t value) {
    assert(!busy && !capture);
    if (fail_open) return false;
    rate = value; changed++; PortAutobaud_start(); return true;
}
static void sof(uint32_t baud, uint32_t start) {
    edge[0] = start; edge[1] = start + (96000000u + baud / 2u) / baud;
    edge[2] = start + (480000000u + baud / 2u) / baud;
    edge[3] = start + (912000000u + baud / 2u) / baud; complete = true;
}
int main(void) {
    if (!port_autobaud_enabled) {
        PortAutobaud_start(); PortAutobaud_error(); PortAutobaud_valid();
        for (unsigned i = 0; i < 1000; i++) assert(!PortAutobaud_poll(i * 10));
        assert(PortAutobaud_waitMs() == UINT32_MAX);
        assert(!capture && !changed && !stopped && !posted && !critical);
        puts("PASS: fixed UART never acquires capture resources or changes baud");
        return 0;
    }
    const uint32_t rates[] = {115200, 460800, 1000000};
    for (unsigned b = 0; b < 3; b++) for (unsigned jitter = 0; jitter < 9; jitter++) {
        sof(rates[b], UINT32_MAX - 13u); edge[1] += jitter; edge[2] += jitter / 2;
        assert(port_autobaud_estimate(edge, 48000000) == rates[b]);
    }
    assert(!port_autobaud_estimate(NULL, 48000000));
    assert(!port_autobaud_estimate(edge, 0));
    for (unsigned b = 0; b < 3; b++) {
        uint32_t kick[] = {0, 48000000u / rates[b], 240000000u / rates[b], 288000000u / rates[b]};
        assert(!port_autobaud_estimate(kick, 48000000)); /* 0xef must never alias 0xfe+LEN=0. */
    }
    uint32_t random = 123;
    unsigned rejected = 0;
    for (unsigned i = 0; i < 100000; i++) {
        random = random * 1664525u + 1013904223u;
        uint32_t noise[] = {0, random & 8191u, (random >> 13) & 65535u, random >> 3};
        rejected += port_autobaud_estimate(noise, 48000000) == 0;
    }
    assert(rejected > 99000);
    for (unsigned b = 0; b < 3; b++) {
        PortAutobaud_start(); complete = false;
        assert(PortAutobaud_waitMs() == 10);
        assert(!PortAutobaud_poll(0) && capture);
        sof(rates[b], 1000);
        busy = true; assert(!PortAutobaud_poll(10) && capture);
        busy = false;
        bool expected_change = rate != rates[b];
        assert(PortAutobaud_poll(20) == expected_change && capture && rate == rates[b]);
        assert(PortAutobaud_waitMs() == 10); /* Timing is only tentative. */
        PortAutobaud_valid(); assert(PortAutobaud_waitMs() == 10 && !capture);
        assert(!PortAutobaud_poll(30));
    }
    /* Even after a good frame the next request is watched, regardless of
     * whether wrong-baud garbage would trigger a hardware framing error. */
    assert(PortAutobaud_waitMs() == 10 && !PortAutobaud_poll(35) && capture);
    PortAutobaud_valid(); assert(!capture && !posted);
    /* UART errors recover detection; no GPIO operation runs in the ISR. */
    unsigned old_stopped = stopped;
    PortAutobaud_error(); assert(stopped == old_stopped && posted == 1);
    assert(PortAutobaud_poll(40) && capture);
    PortAutobaud_valid(); assert(!capture);
    /* Missing/invalid edges and an unavailable peripheral never change baud. */
    unsigned old_changed = changed;
    PortAutobaud_start(); complete = false;
    assert(!PortAutobaud_poll(UINT32_MAX - 20u));
    assert(!PortAutobaud_poll(80) && capture); /* Wrap-safe timeout rearms immediately. */
    PortAutobaud_valid();
    available = false; assert(!PortAutobaud_poll(90) && !capture);
    available = true; assert(!PortAutobaud_poll(100));
    edge[0] = edge[1] = edge[2] = edge[3] = 0; complete = true;
    assert(!PortAutobaud_poll(110) && capture && changed == old_changed);
    assert(!PortAutobaud_poll(120) && capture);
    sof(115200, 500); fail_open = true;
    assert(PortAutobaud_poll(130) && changed == old_changed && capture);
    fail_open = false; assert(!PortAutobaud_poll(140) && capture);
    sof(rate, 0); assert(!PortAutobaud_poll(150) && capture);
    assert(!PortAutobaud_poll(2150)); assert(!PortAutobaud_poll(2160) && capture);
    PortAutobaud_valid(); assert(!capture && !critical);
    rate = 0; PortAutobaud_start();
    assert(!PortAutobaud_poll(2200) && !capture); /* Failed UART owns no DMA power. */
    puts("PASS: passive SOF+LEN=0 baud inference (3 rates/jitter/wrap/100000 noise traces), rejects 0xef alias, continuous capture after confirmation, deferred TX, UART errors, failed open, deadlines and resource contention");
}
