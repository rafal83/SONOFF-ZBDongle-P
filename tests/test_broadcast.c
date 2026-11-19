#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "port_broadcast.h"
uint8_t NWK_TaskID = 3;
static unsigned critical, events;
uint32_t OsalPort_enterCS(void) { return critical++; }
void OsalPort_leaveCS(uint32_t key) { critical = key; }
uint8_t OsalPort_setEvent(uint8_t task, uint32_t event) {
    assert(task == NWK_TaskID && event == 0x2000 && !critical); events++; return 0;
}
int main(void) {
    uint8_t messages[PORT_BROADCAST_CAPACITY + 1];
    assert(!port_broadcast_receive() && port_broadcast_send(NULL));
    for (unsigned round = 0; round < 10000; round++) {
        for (unsigned i = 0; i < PORT_BROADCAST_CAPACITY; i++) assert(!port_broadcast_send(messages + i));
        assert(port_broadcast_send(messages + PORT_BROADCAST_CAPACITY));
        for (unsigned i = 0; i < PORT_BROADCAST_CAPACITY; i++) assert(port_broadcast_receive() == messages + i);
        assert(!port_broadcast_receive());
    }
    /* One dequeue then enqueue at every ring offset: accepted messages retain
     * order. No allocator or free API exists in this translation unit. */
    for (unsigned i = 0; i < PORT_BROADCAST_CAPACITY; i++) assert(!port_broadcast_send(messages + i));
    for (unsigned i = 0; i < 100000; i++) {
        unsigned index = i % PORT_BROADCAST_CAPACITY;
        assert(port_broadcast_receive() == messages + index);
        assert(!port_broadcast_send(messages + index));
    }
    for (unsigned i = 0; i < PORT_BROADCAST_CAPACITY; i++) assert(port_broadcast_receive() == messages + i);
    assert(!port_broadcast_receive() && !critical && events);
    assert(port_broadcast_stats.accepted == 200010 && port_broadcast_stats.dropped == 10000);
    assert(port_broadcast_stats.high_water == PORT_BROADCAST_CAPACITY);
    puts("PASS: fixed native-boundary broadcast FIFO, 200010 transfers, saturation ownership, wrap order, event coalescing rearm, no allocation/free");
}
