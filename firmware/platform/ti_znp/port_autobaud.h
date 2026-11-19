/* SPDX-License-Identifier: MIT */
#ifndef PORT_TI_AUTOBAUD_H
#define PORT_TI_AUTOBAUD_H
#include <stdbool.h>
#include <stdint.h>
void PortAutobaud_start(void);
void PortAutobaud_error(void); /* UART ISR: notify task only. */
void PortAutobaud_valid(void); /* Task: real MT frame passed framing/FCS. */
bool PortAutobaud_poll(uint32_t now_ms); /* True: discard damaged/stale RX. */
uint32_t PortAutobaud_waitMs(void);
uint32_t NPITLUART_getBaud(void);
bool NPITLUART_setBaud(uint32_t rate); /* NPI task, no TX in progress. */
#endif
