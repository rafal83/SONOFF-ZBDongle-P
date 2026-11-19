/* SPDX-License-Identifier: MIT */
#ifndef PORT_TI_NPI_H
#define PORT_TI_NPI_H
#include <stdint.h>
#include "npi_frame.h"
extern const uint32_t port_npi_baudrate;
void PortNPI_initialize(npiIncomingFrameCBack_t receive);
void PortNPI_collect(void);
uint32_t PortNPI_waitTicks(void);
void PortNPI_sendToHost(uint8_t *message);
void PortNPI_pumpTx(void);
#endif
