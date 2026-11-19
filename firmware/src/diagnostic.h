/* SPDX-License-Identifier: MIT */
#ifndef PORT_DIAGNOSTIC_H
#define PORT_DIAGNOSTIC_H
#include "npi.h"
typedef struct {
    npi_tx_queue tx;
    uint8_t ieee_le[8];
    uint32_t unsupported, ignored;
} diagnostic;
void diagnostic_init(diagnostic *device, const uint8_t ieee_le[8]);
void diagnostic_receive(void *context, const npi_frame *request);
#endif
