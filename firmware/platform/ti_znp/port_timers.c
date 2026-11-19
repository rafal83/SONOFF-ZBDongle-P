/* SPDX-License-Identifier: MIT */
#include "port_timers.h"
#include <ti/drivers/dpl/ClockP.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    ClockP_Handle clock; /* Preserve the TI ScheduleEntry prefix. */
    OsalPort_TimerCback callback;
    void *arg;
    uint32_t event;
    ClockP_Struct object;
    uint8_t task, flags;
} timer_slot;
#define EVENT 1u
#define RELOAD 2u
#define RETIRED 4u
#define RUNNING 8u
static timer_slot timers[PORT_TIMER_SLOTS];
static uint32_t free_slots = (1u << PORT_TIMER_SLOTS) - 1u;
static uint32_t retired_slots, cleanup_event;
static uint8_t cleanup_task;
uint32_t port_timer_active, port_timer_high_water, port_timer_failures;
_Static_assert(PORT_TIMER_SLOTS < 32, "Bitmap size exceeded");
static timer_slot *lookup(void *handle, unsigned *index) {
    uintptr_t value = (uintptr_t)handle, base = (uintptr_t)timers;
    if (value < base || value >= base + sizeof(timers) || (value - base) % sizeof(timer_slot)) return NULL;
    *index = (unsigned)((value - base) / sizeof(timer_slot));
    return (free_slots & (1u << *index)) || (timers[*index].flags & RETIRED) ? NULL : &timers[*index];
}
static void timer_callback(uintptr_t arg) {
    timer_slot *slot = (timer_slot *)arg;
    uint32_t key = OsalPort_enterCS();
    unsigned index;
    bool valid = lookup(slot, &index) != NULL;
    if (valid) slot->flags |= RUNNING;
    OsalPort_TimerCback callback = valid ? slot->callback : NULL;
    void *context = valid ? slot->arg : NULL;
    bool event_timer = valid && (slot->flags & EVENT);
    uint8_t task = valid ? slot->task : 0;
    uint32_t event = valid ? slot->event : 0;
    if (event_timer && !(slot->flags & RELOAD)) {
        ClockP_stop(slot->clock);
        slot->flags |= RETIRED; retired_slots |= 1u << index; port_timer_active--;
    }
    OsalPort_leaveCS(key);
    if (event_timer) OsalPort_setEvent(task, event);
    else if (callback) callback(context); /* May stop/delete/rearm itself. */
    key = OsalPort_enterCS();
    if (valid) slot->flags &= ~RUNNING;
    bool retired = valid && (slot->flags & RETIRED);
    OsalPort_leaveCS(key);
    if (retired && cleanup_event) OsalPort_setEvent(cleanup_task, cleanup_event);
}
uint8_t PortTimer_set(uint32_t ticks, OsalPort_TimerCback callback, void *arg, OsalPort_TimerID *handle) {
    if (!handle || !callback) return OsalPort_INVALIDPARAMETER;
    uint32_t key = OsalPort_enterCS();
    unsigned index;
    timer_slot *slot;
    if (!*handle) {
        if (!free_slots) { port_timer_failures++; OsalPort_leaveCS(key); return OsalPort_NO_TIMER_AVAIL; }
        index = (unsigned)__builtin_ctz(free_slots);
        free_slots &= ~(1u << index);
        slot = &timers[index];
        slot->flags = 0;
        ClockP_Params params;
        ClockP_Params_init(&params);
        params.startFlag = false; params.period = 0; params.arg = (uintptr_t)slot;
        slot->clock = ClockP_construct(&slot->object, timer_callback, ticks, &params);
        if (!slot->clock) {
            free_slots |= 1u << index; port_timer_failures++;
            OsalPort_leaveCS(key); return OsalPort_NO_TIMER_AVAIL;
        }
        port_timer_active++;
        if (port_timer_active > port_timer_high_water) port_timer_high_water = port_timer_active;
        *handle = slot;
    } else {
        slot = lookup(*handle, &index);
        if (!slot) { OsalPort_leaveCS(key); return OsalPort_INVALIDPARAMETER; }
        ClockP_stop(slot->clock);
    }
    slot->callback = callback; slot->arg = arg;
    ClockP_setTimeout(slot->clock, ticks);
    ClockP_start(slot->clock);
    OsalPort_leaveCS(key);
    return OsalPort_SUCCESS;
}
uint8_t PortTimer_delete(OsalPort_TimerID *handle) {
    if (!handle) return OsalPort_INVALIDPARAMETER;
    uint32_t key = OsalPort_enterCS(); unsigned index;
    timer_slot *slot = lookup(*handle, &index);
    if (!slot) { OsalPort_leaveCS(key); return OsalPort_INVALIDPARAMETER; }
    ClockP_stop(slot->clock);
    if (slot->flags & RUNNING) {
        slot->flags |= RETIRED; retired_slots |= 1u << index;
        *handle = NULL; port_timer_active--;
        OsalPort_leaveCS(key); return OsalPort_SUCCESS;
    }
    ClockP_destruct(&slot->object);
    slot->clock = NULL; slot->callback = NULL; slot->arg = NULL;
    *handle = NULL;
    free_slots |= 1u << index; port_timer_active--;
    OsalPort_leaveCS(key);
    return OsalPort_SUCCESS;
}
void PortTimer_stop(OsalPort_TimerID *handle) {
    if (!handle) return;
    uint32_t key = OsalPort_enterCS(); unsigned index;
    timer_slot *slot = lookup(*handle, &index);
    if (slot) ClockP_stop(slot->clock);
    OsalPort_leaveCS(key);
}
static timer_slot *find_event(uint8_t task, uint32_t event) {
    /* Bounded to 24 static slots, independent of network/heap fragmentation. */
    for (unsigned i = 0; i < PORT_TIMER_SLOTS; i++)
        if (!(free_slots & (1u << i)) && (timers[i].flags & (EVENT | RETIRED)) == EVENT &&
            timers[i].task == task && timers[i].event == event) return &timers[i];
    return NULL;
}
static void event_callback_unused(void *arg) { (void)arg; }
uint8_t PortEventTimer_start(uint8_t task, uint32_t event, uint32_t ms, bool reload) {
    uint32_t period = ClockP_getSystemTickPeriod();
    uint64_t ticks64 = ((uint64_t)ms * 1000u + period - 1u) / period;
    if (ticks64 > UINT32_MAX) return OsalPort_INVALIDPARAMETER;
    uint32_t ticks = ticks64 ? (uint32_t)ticks64 : 1;
    uint32_t key = OsalPort_enterCS();
    timer_slot *slot = find_event(task, event);
    void *handle = slot;
    if (slot && !!(slot->flags & RELOAD) != reload) {
        (void)PortTimer_delete(&handle); slot = NULL;
    }
    uint8_t status = PortTimer_set(ticks, event_callback_unused, NULL, &handle);
    if (!status) {
        slot = handle;
        ClockP_stop(slot->clock);
        slot->flags = EVENT | (reload ? RELOAD : 0);
        slot->task = task; slot->event = event;
        if (reload) ClockP_setPeriod(slot->clock, ticks);
        ClockP_start(slot->clock);
    }
    OsalPort_leaveCS(key); return status;
}
uint8_t PortEventTimer_stop(uint8_t task, uint32_t event) {
    uint32_t key = OsalPort_enterCS();
    void *handle = find_event(task, event);
    uint8_t status = handle ? PortTimer_delete(&handle) : OsalPort_INVALIDPARAMETER;
    OsalPort_leaveCS(key); return status;
}
uint32_t PortEventTimer_timeout(uint8_t task, uint32_t event) {
    uint32_t key = OsalPort_enterCS();
    timer_slot *slot = find_event(task, event);
    uint32_t ticks = slot ? ClockP_getTimeout(slot->clock) : 0;
    OsalPort_leaveCS(key);
    return (uint32_t)(((uint64_t)ticks * ClockP_getSystemTickPeriod()) / 1000u);
}
void PortEventTimer_cleanup(void) {
    uint32_t key = OsalPort_enterCS(); uint32_t pending = retired_slots;
    while (pending) {
        unsigned index = (unsigned)__builtin_ctz(pending); uint32_t bit = 1u << index;
        pending &= ~bit;
        if (timers[index].flags & RUNNING) continue;
        ClockP_destruct(&timers[index].object);
        timers[index].clock = NULL; timers[index].flags = 0;
        retired_slots &= ~bit; free_slots |= bit;
    }
    OsalPort_leaveCS(key);
}
void PortEventTimer_register(uint8_t task, uint32_t event) { cleanup_task = task; cleanup_event = event; }
