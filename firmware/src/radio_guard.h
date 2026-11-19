/* SPDX-License-Identifier: MIT */
#ifndef PORT_RADIO_GUARD_H
#define PORT_RADIO_GUARD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Structural checks before the native TI MAC callback/ NWK parsers.
 * Does not authenticate, decrypt or suppress duplicate frames.
 * Caller serializes access (the Z-Stack task in the firmware).
 */
extern uint32_t port_radio_rejected;
/* Size, rejection class and two NWK frame-control bytes only; no payload,
 * key, security counter or address is retained. */
extern uint32_t port_radio_last_rejection;
bool port_radio_guard_accept(const uint8_t *frame, size_t size, uint8_t security_level);
#endif
