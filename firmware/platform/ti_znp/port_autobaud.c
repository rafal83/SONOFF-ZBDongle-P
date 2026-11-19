/* SPDX-License-Identifier: MIT
 * Passive SOF timing followed by ordinary MT validation; no synthetic ping,
 * baud cycling, flash writes or changes while a frame is being transmitted.
 */
#include "port_autobaud.h"
#include "port_baud_capture.h"
#include "autobaud.h"
#include "npi_tl.h"
#include "osal_port.h"
#include <ti/sysbios/knl/Semaphore.h>
extern Semaphore_Handle npiSemHandle;
extern const uint32_t port_autobaud_enabled;
enum { ARM, CAPTURE };
static uint32_t since;
static uint8_t phase;
static volatile uint8_t error;
void PortAutobaud_start(void) { if (phase == CAPTURE) PortBaudCapture_stop(); phase = ARM; }
void PortAutobaud_error(void) { if (port_autobaud_enabled) { error = 1; Semaphore_post(npiSemHandle); } }
void PortAutobaud_valid(void) {
    if (phase == CAPTURE) PortBaudCapture_stop();
    phase = ARM;
}
uint32_t PortAutobaud_waitMs(void) { return port_autobaud_enabled ? 10u : UINT32_MAX; }
bool PortAutobaud_poll(uint32_t now) {
    if (!port_autobaud_enabled) return false;
    bool discard = false;
    uint32_t key = OsalPort_enterCS();
    if (error) {
        error = 0;
        discard = true;
    }
    OsalPort_leaveCS(key);
    if (phase == CAPTURE) {
        uint32_t edges[4];
        if (PortBaudCapture_read(edges)) {
            uint32_t rate = port_autobaud_estimate(edges, PortBaudCapture_clock());
            if (rate && NPITL_checkNpiBusy()) return discard;
            PortBaudCapture_stop();
            phase = ARM;
            since = now;
            if (rate && rate != NPITLUART_getBaud()) {
                discard = true;
                if (!NPITLUART_setBaud(rate)) phase = ARM;
                else phase = ARM;
            }
        } else if ((uint32_t)(now - since) >= 100u) {
            PortBaudCapture_stop(); phase = ARM;
        }
    }
    /* Monitor even a working UART so the FIRST ping at a different speed is
     * measured. Re-arm in this call rather than leave a 10 ms blind window.
     * UART owns DMA power: never acquire after a failed UART open. */
    if (phase == ARM && NPITLUART_getBaud() && PortBaudCapture_start()) { phase = CAPTURE; since = now; }
    return discard;
}
