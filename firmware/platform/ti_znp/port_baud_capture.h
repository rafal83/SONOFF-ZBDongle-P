/* SPDX-License-Identifier: MIT */
#ifndef PORT_BAUD_CAPTURE_H
#define PORT_BAUD_CAPTURE_H
#include <stdbool.h>
#include <stdint.h>
bool PortBaudCapture_start(void);
void PortBaudCapture_stop(void);
bool PortBaudCapture_read(uint32_t edges[4]);
uint32_t PortBaudCapture_clock(void);
#endif
