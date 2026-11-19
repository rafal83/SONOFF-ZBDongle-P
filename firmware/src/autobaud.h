/* SPDX-License-Identifier: MIT */
#ifndef PORT_AUTOBAUD_H
#define PORT_AUTOBAUD_H
#include <stdint.h>
/* Four successive RX transitions, beginning at idle-high: the MT SOF 0xfe
 * has a two-bit low interval followed by eight high bits (including stop).
 * followed by LEN=0 has a nine-bit low interval. This rejects 0xef, the
 * bootloader kick, which aliases the first three edges at twice its baud.
 * This only proposes a rate. A real UART frame/FCS must confirm it. */
uint32_t port_autobaud_estimate(const uint32_t edges[4], uint32_t clock_hz);
#endif
