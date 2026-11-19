/* SPDX-License-Identifier: MIT */
#include "diagnostic.h"
void diagnostic_init(diagnostic *d, const uint8_t ieee[8]) {
    *d = (diagnostic){0};
    npi_tx_init(&d->tx);
    for (size_t i = 0; i < 8; i++) d->ieee_le[i] = ieee[i];
}
void diagnostic_receive(void *context, const npi_frame *r) {
    diagnostic *d = context;
    /* AREQ/reset/NV writes are deliberately not implemented. */
    if ((r->cmd0 & 0xe0u) != 0x20u) { d->ignored++; return; }
    /* Serialize only initialized payload bytes; avoid clearing 250 bytes for ping. */
    npi_frame reply;
    reply.cmd0 = 0x61;
    reply.cmd1 = r->cmd1;
    reply.length = 0;
    uint8_t error = 0;
    if ((r->cmd0 & 0x1fu) != 1) error = 1; /* MT_RPC_ERR_SUBSYSTEM */
    else if (r->cmd1 != 1 && r->cmd1 != 2 && r->cmd1 != 4) error = 2;
    else if (r->length) error = 4; /* MT_RPC_ERR_LENGTH */
    else if (r->cmd1 == 1) {
        reply.length = 2;
        reply.payload[0] = 1; /* SYS only: no AF/ZDO or coordinator capability */
        reply.payload[1] = 0;
    } else if (r->cmd1 == 2) {
        reply.length = 9;
        for (unsigned i = 0; i < reply.length; i++) reply.payload[i] = 0;
        reply.payload[0] = 2;
        reply.payload[1] = 0xfe; /* experimental product, NOT ZStack3x0 */
        reply.payload[3] = 1;    /* diagnostic version 0.1.0, revision 0 */
    } else {
        reply.length = 8;
        for (size_t i = 0; i < 8; i++) reply.payload[i] = d->ieee_le[i];
    }
    if (error) {
        d->unsupported++;
        reply.cmd0 = 0x60; /* SRSP RPC error subsystem */
        reply.cmd1 = 0;
        reply.length = 3;
        reply.payload[0] = error;
        reply.payload[1] = r->cmd0;
        reply.payload[2] = r->cmd1;
    }
    (void)npi_tx_enqueue(&d->tx, &reply);
}
