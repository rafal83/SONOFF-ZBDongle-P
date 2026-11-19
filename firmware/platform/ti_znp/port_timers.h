/* SPDX-License-Identifier: MIT */
#ifndef PORT_TIMERS_H
#define PORT_TIMERS_H
#include <stdbool.h>
#include "osal_port.h"
#define PORT_TIMER_SLOTS 24u
uint8_t PortTimer_set(uint32_t ticks, OsalPort_TimerCback callback, void *arg, OsalPort_TimerID *handle);
uint8_t PortTimer_delete(OsalPort_TimerID *handle);
void PortTimer_stop(OsalPort_TimerID *handle);
uint8_t PortEventTimer_start(uint8_t task, uint32_t event, uint32_t ms, bool reload);
uint8_t PortEventTimer_stop(uint8_t task, uint32_t event);
uint32_t PortEventTimer_timeout(uint8_t task, uint32_t event);
void PortEventTimer_cleanup(void);
void PortEventTimer_register(uint8_t task, uint32_t event);
#endif
