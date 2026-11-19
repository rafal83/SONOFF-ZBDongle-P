/* SPDX-License-Identifier: MIT */
#ifndef PORT_WATCHDOG_H
#define PORT_WATCHDOG_H
#include <stdbool.h>
#include <stdint.h>
#define PORT_WATCHDOG_NPI 1u
#define PORT_WATCHDOG_STACK 2u
bool PortWatchdog_start(void);
void PortWatchdog_progress(uint8_t participant);
uint32_t PortWatchdog_waitTicks(uint32_t requested);
#endif
