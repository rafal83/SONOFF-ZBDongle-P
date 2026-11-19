/* SPDX-License-Identifier: MIT; PC-only transport fixture, no serial or sockets. */
#include "diagnostic.h"
#include <stdio.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
int main(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const uint8_t synthetic_ieee[8] = {1, 0, 0, 0, 0, 0, 0, 2};
    diagnostic d;
    npi_parser p;
    diagnostic_init(&d, synthetic_ieee);
    npi_init(&p, diagnostic_receive, &d);
    int input;
    while ((input = getchar()) != EOF) {
        npi_feed(&p, (uint8_t)input, 0);
        uint8_t byte;
        while (npi_tx_peek(&d.tx, &byte)) {
            if (putchar(byte) == EOF) return 1;
            npi_tx_consume(&d.tx);
        }
    }
    return ferror(stdin) || fflush(stdout) == EOF;
}
