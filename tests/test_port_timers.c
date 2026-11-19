#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "port_timers.h"
#include <ti/drivers/dpl/ClockP.h>
typedef struct { ClockP_Fxn callback; ClockP_Params params; uint32_t timeout; bool constructed, active; } mock_clock;
static mock_clock *clocks[PORT_TIMER_SLOTS];
static unsigned cs, fired, constructed;
static bool reject_construct;
static uint32_t events[256];
uint8_t OsalPort_setEvent(uint8_t task, uint32_t event) { events[task] |= event; return 0; }
uint32_t ClockP_getSystemTickPeriod(void) { return 10; }
extern uint32_t port_timer_active;
uint32_t OsalPort_enterCS(void) { return cs++; }
void OsalPort_leaveCS(uint32_t key) { cs = key; }
void ClockP_Params_init(ClockP_Params *p) { memset(p, 0, sizeof(*p)); }
ClockP_Handle ClockP_construct(ClockP_Struct *s, ClockP_Fxn cb, uint32_t ticks, ClockP_Params *p) {
    assert(cs && !p->startFlag && !p->period);
    if (reject_construct) return NULL;
    mock_clock *c = (mock_clock *)s;
    *c = (mock_clock){ .callback = cb, .params = *p, .timeout = ticks, .constructed = true };
    bool known = false;
    for (unsigned i = 0; i < constructed; i++) if (clocks[i] == c) known = true;
    if (!known) clocks[constructed++] = c;
    return c;
}
void ClockP_destruct(ClockP_Struct *s) { mock_clock *c = (mock_clock *)s; assert(c->constructed && !c->active); c->constructed = false; }
void ClockP_stop(ClockP_Handle h) { mock_clock *c = h; assert(c->constructed); c->active = false; }
void ClockP_start(ClockP_Handle h) { mock_clock *c = h; assert(c->constructed); c->active = true; }
void ClockP_setTimeout(ClockP_Handle h, uint32_t n) { mock_clock *c = h; assert(c->constructed && !c->active); c->timeout = n; }
void ClockP_setPeriod(ClockP_Handle h, uint32_t n) { mock_clock *c = h; assert(c->constructed && !c->active && n); c->params.period = n; }
uint32_t ClockP_getTimeout(ClockP_Handle h) { return ((mock_clock *)h)->timeout; }
static void callback(void *p) { fired += *(unsigned *)p; }
static void self_delete(void *p) { assert(PortTimer_delete(p) == 0); fired++; }
int main(void) {
    _Static_assert(sizeof(mock_clock) <= sizeof(ClockP_Struct), "Mock clock storage insufficient");
    OsalPort_TimerID handles[PORT_TIMER_SLOTS + 1] = {0};
    unsigned one = 1, two = 2;
    assert(PortTimer_set(1, callback, &one, NULL) == OsalPort_INVALIDPARAMETER);
    reject_construct = true;
    assert(PortTimer_set(1, callback, &one, &handles[0]) == OsalPort_NO_TIMER_AVAIL && !handles[0]);
    reject_construct = false;
    PortEventTimer_register(14, 0x80000000u);
    for (unsigned i = 0; i < PORT_TIMER_SLOTS; i++) assert(PortTimer_set(i + 1, callback, &one, &handles[i]) == 0);
    assert(port_timer_active == PORT_TIMER_SLOTS);
    assert(PortTimer_set(1, callback, &one, &handles[PORT_TIMER_SLOTS]) == OsalPort_NO_TIMER_AVAIL);
    assert(PortTimer_set(55, callback, &two, &handles[0]) == 0);
    clocks[0]->callback(clocks[0]->params.arg); assert(fired == 2);
    PortTimer_stop(&handles[0]); assert(!clocks[0]->active);
    for (unsigned i = 0; i < PORT_TIMER_SLOTS; i++) assert(PortTimer_delete(&handles[i]) == 0 && !handles[i]);
    assert(port_timer_active == 0);
    for (unsigned i = 0; i < 1000; i++) {
        assert(PortTimer_set(1, self_delete, &handles[0], &handles[0]) == 0);
        clocks[0]->callback(clocks[0]->params.arg); assert(!handles[0] && !port_timer_active);
        assert(clocks[0]->constructed); /* Destruction deferred beyond SWI. */
        PortEventTimer_cleanup();
    }
    handles[0] = (void *)1;
    assert(PortTimer_delete(&handles[0]) == OsalPort_INVALIDPARAMETER);
    assert(PortTimer_set(1, callback, &one, &handles[0]) == OsalPort_INVALIDPARAMETER);
    assert(!cs && fired == 1002);
    assert(PortEventTimer_start(2, 0x24, 123, false) == 0);
    assert(PortEventTimer_timeout(2, 0x24) == 123);
    clocks[0]->callback(clocks[0]->params.arg);
    assert(events[2] == 0x24 && events[14] == 0x80000000u && port_timer_active == 0);
    assert(clocks[0]->constructed);
    PortEventTimer_cleanup(); assert(!clocks[0]->constructed);
    assert(PortEventTimer_start(2, 0x24, 20, true) == 0);
    assert(clocks[0]->params.period == 2000);
    assert(PortEventTimer_start(2, 0x24, 40, true) == 0);
    assert(clocks[0]->params.period == 4000 && port_timer_active == 1);
    clocks[0]->callback(clocks[0]->params.arg); assert(port_timer_active == 1);
    assert(PortEventTimer_start(2, 0x24, 50, false) == 0 && clocks[0]->params.period == 0);
    assert(PortEventTimer_stop(2, 0x24) == 0 && !port_timer_active);
    assert(PortEventTimer_start(1, 1, UINT32_MAX, true) == OsalPort_INVALIDPARAMETER);
    assert(!cs);
    puts("PASS: fixed OSAL timers, no malloc/create/delete API, saturation, construct failure rollback, callback update/self-delete, 1000 reuses, invalid handles");
    return 0;
}
