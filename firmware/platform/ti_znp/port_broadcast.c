/* SPDX-License-Identifier: MIT
 * Independent fixed FIFO at the public NWK broadcast queue boundary. Leave
 * validation, decryption, duplicate tracking, routing and passive ACKs to TI.
 */
#include "port_broadcast.h"
#include "nwk.h"
#include "nwk_globals.h"
#include "osal_port.h"
#include "zcomdef.h"
_Static_assert(PORT_BROADCAST_CAPACITY == MAX_BROADCAST_QUEUED,
               "The fixed FIFO must match the native broadcast admission limit");
extern uint8_t NWK_TaskID;
static uint8_t *queue[PORT_BROADCAST_CAPACITY];
static uint8_t head, count;
port_broadcast_stats_t port_broadcast_stats;
uint8_t port_broadcast_send(uint8_t *message) {
    if (!message) return INVALIDPARAMETER;
    uint32_t key = OsalPort_enterCS();
    if (count == PORT_BROADCAST_CAPACITY) {
        port_broadcast_stats.dropped++;
        OsalPort_leaveCS(key);
        return ZMemError;
    }
    queue[(head + count) % PORT_BROADCAST_CAPACITY] = message;
    count++;
    if (count > port_broadcast_stats.high_water) port_broadcast_stats.high_water = count;
    port_broadcast_stats.accepted++;
    OsalPort_leaveCS(key);
    OsalPort_setEvent(NWK_TaskID, NWK_BROADCAST_MSG_EVT);
    return SUCCESS;
}
uint8_t *port_broadcast_receive(void) {
    uint32_t key = OsalPort_enterCS();
    if (!count) { OsalPort_leaveCS(key); return 0; }
    uint8_t *message = queue[head];
    head = (head + 1u) % PORT_BROADCAST_CAPACITY;
    count--;
    uint8_t remaining = count;
    OsalPort_leaveCS(key);
    /* Task events coalesce. The stack scheduler clears its snapshot BEFORE
     * invoking the native handler, so this post retains another ready turn. */
    if (remaining) OsalPort_setEvent(NWK_TaskID, NWK_BROADCAST_MSG_EVT);
    return message;
}
