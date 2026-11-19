/* SPDX-License-Identifier: MIT */
#ifndef PORT_BROADCAST_H
#define PORT_BROADCAST_H
#include <stdint.h>
#define PORT_BROADCAST_CAPACITY 10u
typedef struct { uint32_t accepted, dropped; uint8_t high_water; } port_broadcast_stats_t;
extern port_broadcast_stats_t port_broadcast_stats;
/* Implements the public nwk_bufs.h queue contract. Ownership passes only on
 * SUCCESS. Receive transfers ownership back to the native NWK consumer. */
uint8_t port_broadcast_send(uint8_t *message);
uint8_t *port_broadcast_receive(void);
#endif
