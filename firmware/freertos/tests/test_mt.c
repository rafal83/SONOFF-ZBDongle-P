/* SPDX-License-Identifier: MIT
 * MT dispatcher through the real NPI parser and encoder (firmware/src/npi.c).
 */
#include "frt_test.h"
#include "mt.h"

static int resets = -1;
static int diag_calls;
static npi_frame last;
static int responses;

static void ieee(void *ctx, uint8_t out[8]) {
    (void)ctx;
    for (unsigned i = 0; i < 8; i++) out[i] = (uint8_t)(0x10 + i);
}
static void reset(void *ctx, uint8_t type) { (void)ctx; resets = type; }
static int diag(void *ctx, uint8_t cmd, const uint8_t *in, uint8_t len, uint8_t *out, uint8_t cap) {
    (void)ctx;
    (void)cap;
    diag_calls++;
    if (cmd == 0x00) { out[0] = 0xAB; return 1; }
    if (cmd == 0x01) return len == 1 ? (out[0] = in[0], 1) : MT_DIAG_BAD_PARAM;
    if (cmd == DIAG_PERF) { for (unsigned i = 0; i < 225u && i < cap; i++) out[i] = (uint8_t)i; return 225; }
    return MT_DIAG_UNKNOWN;
}
static const mt_platform_t P = {0, ieee, reset, diag, 20261118u};

static void on_frame(void *ctx, const npi_frame *req) {
    (void)ctx;
    if (mt_dispatch(&P, req, &last)) responses++;
}

/* Encode -> byte stream -> parser -> dispatcher. */
static void send(npi_parser *parser, uint8_t cmd0, uint8_t cmd1, const uint8_t *payload, uint8_t len) {
    npi_frame f = {.cmd0 = cmd0, .cmd1 = cmd1, .length = len};
    for (unsigned i = 0; i < len; i++) f.payload[i] = payload[i];
    uint8_t wire[NPI_MAX_FRAME];
    size_t n = npi_encode(wire, sizeof wire, &f);
    CHECK(n == len + 5u);
    for (size_t i = 0; i < n; i++) npi_feed(parser, wire[i], 0);
}

int main(void) {
    npi_parser parser;
    npi_init(&parser, on_frame, 0);

    send(&parser, 0x21, 0x01, 0, 0);              /* SYS_PING */
    CHECK(responses == 1 && last.cmd0 == 0x61 && last.cmd1 == 0x01 && last.length == 2);
    CHECK(last.payload[0] == 0x01 && last.payload[1] == 0x00);   /* SYS capability only */

    send(&parser, 0x21, 0x02, 0, 0);              /* SYS_VERSION */
    CHECK(last.cmd0 == 0x61 && last.length == 9 && last.payload[1] == FRT_PRODUCT_ID);
    CHECK(last.payload[1] != 1 && last.payload[1] != 2);          /* never ZStack3x0/30x */
    CHECK(last.payload[5] == 0xFE && last.payload[6] == 0x28 && last.payload[8] == 0x01); /* 20261118 = 0x013528FE LE */

    send(&parser, 0x21, 0x04, 0, 0);              /* SYS_GET_EXTADDR */
    CHECK(last.length == 8 && last.payload[0] == 0x10 && last.payload[7] == 0x17);

    const uint8_t extra = 1;
    send(&parser, 0x21, 0x01, &extra, 1);         /* bad length */
    CHECK(last.cmd0 == 0x60 && last.cmd1 == 0 && last.payload[0] == MT_RPC_ERR_LENGTH);
    CHECK(last.payload[1] == 0x21 && last.payload[2] == 0x01);

    send(&parser, 0x21, 0x09, 0, 0);              /* SYS_OSAL_NV_READ: not implemented */
    CHECK(last.cmd0 == 0x60 && last.payload[0] == MT_RPC_ERR_COMMAND_ID);

    send(&parser, 0x24, 0x01, 0, 0);              /* AF_DATA_REQUEST: no AF subsystem */
    CHECK(last.cmd0 == 0x60 && last.payload[0] == MT_RPC_ERR_SUBSYSTEM && last.payload[1] == 0x24);
    send(&parser, 0x25, 0x40, 0, 0);              /* ZDO_STARTUP_FROM_APP */
    CHECK(last.payload[0] == MT_RPC_ERR_SUBSYSTEM);
    send(&parser, 0x2F, 0x00, 0, 0);              /* APP_CNF */
    CHECK(last.payload[0] == MT_RPC_ERR_SUBSYSTEM);

    int before = responses;
    send(&parser, 0x44, 0x80, 0, 0);              /* AREQ on another subsystem: ignored */
    send(&parser, 0x41, 0x99, 0, 0);              /* unknown SYS AREQ: ignored */
    CHECK(responses == before && resets == -1);
    send(&parser, 0x41, 0x00, 0, 0);              /* SYS_RESET_REQ without type */
    CHECK(resets == -1 && responses == before);
    const uint8_t soft = 1;
    send(&parser, 0x41, 0x00, &soft, 1);          /* SYS_RESET_REQ: no SRSP */
    CHECK(resets == 1 && responses == before);

    send(&parser, 0x3F, 0x00, 0, 0);              /* private diag */
    CHECK(last.cmd0 == 0x7F && last.cmd1 == 0x00 && last.length == 1 && last.payload[0] == 0xAB);
    const uint8_t idx = 7;
    send(&parser, 0x3F, 0x01, &idx, 1);
    CHECK(last.cmd0 == 0x7F && last.payload[0] == 7);
    send(&parser, 0x3F, 0x01, 0, 0);
    CHECK(last.cmd0 == 0x60 && last.payload[0] == MT_RPC_ERR_PARAMETER);
    send(&parser, 0x3F, 0x55, 0, 0);
    CHECK(last.cmd0 == 0x60 && last.payload[0] == MT_RPC_ERR_COMMAND_ID);
    CHECK(diag_calls == 4);

    npi_frame ind;
    mt_build_reset_ind(&ind, 2);
    CHECK(ind.cmd0 == 0x41 && ind.cmd1 == 0x80 && ind.length == 6 && ind.payload[0] == 2);
    CHECK(ind.payload[2] == FRT_PRODUCT_ID);
    CHECK(mt_reset_reason(0, false) == 0 && mt_reset_reason(1, false) == 1);
    CHECK(mt_reset_reason(4, false) == 0 && mt_reset_reason(2, false) == 0);
    CHECK(mt_reset_reason(7, false) == 1 && mt_reset_reason(7, true) == 2);
    CHECK(parser.bad_fcs == 0 && parser.accepted == (uint32_t)(responses + 4)); /* 4 AREQ without reply */

    /* Diagnostics as read-only NV items (SYS osalNvRead / ReadExt / Length). */
    {
        npi_frame req = {.cmd0 = 0x21, .cmd1 = MT_SYS_OSAL_NV_READ, .length = 3}, rsp;
        req.payload[0] = 0xA0; req.payload[1] = 0x0F; req.payload[2] = 0;          /* 0x0FA0: DIAG_INFO */
        CHECK(mt_diag_nv(&P, &req, &rsp) && rsp.cmd0 == 0x61 && rsp.cmd1 == 0x08 && rsp.length == 3 &&
              rsp.payload[0] == 0 && rsp.payload[1] == 1 && rsp.payload[2] == 0xAB);
        req.payload[0] = 0xA5;                                                       /* 0x0FA5: DIAG_PERF */
        CHECK(mt_diag_nv(&P, &req, &rsp) && rsp.length == 227 && rsp.payload[1] == 225 && rsp.payload[2] == 0 &&
              rsp.payload[226] == 224);
        req.payload[2] = 200;                                                        /* offset */
        CHECK(mt_diag_nv(&P, &req, &rsp) && rsp.payload[0] == 0 && rsp.payload[1] == 25 && rsp.payload[2] == 200);
        req.payload[2] = 225;                                                        /* past the end */
        CHECK(mt_diag_nv(&P, &req, &rsp) && rsp.length == 2 && rsp.payload[0] == MT_NV_OPER_FAILED);
        npi_frame ext = {.cmd0 = 0x21, .cmd1 = MT_SYS_OSAL_NV_READ_EXT, .length = 4};
        ext.payload[0] = 0xA5; ext.payload[1] = 0x0F; ext.payload[2] = 10; ext.payload[3] = 0;
        CHECK(mt_diag_nv(&P, &ext, &rsp) && rsp.cmd1 == 0x1C && rsp.payload[1] == 215 && rsp.payload[2] == 10);
        npi_frame len = {.cmd0 = 0x21, .cmd1 = MT_SYS_OSAL_NV_LENGTH, .length = 2};
        len.payload[0] = 0xA5; len.payload[1] = 0x0F;
        CHECK(mt_diag_nv(&P, &len, &rsp) && rsp.length == 2 && rsp.payload[0] == 225 && rsp.payload[1] == 0);
        len.payload[0] = 0xA7;                                                       /* block the platform lacks */
        CHECK(mt_diag_nv(&P, &len, &rsp) && rsp.payload[0] == 0 && rsp.payload[1] == 0);
        /* not ours: other items, other commands, writes (the ZNP layer refuses those) */
        len.payload[0] = 0xA9;                                                       /* 0x0FA9: DIAG_HOPS, the last */
        CHECK(mt_diag_nv(&P, &len, &rsp));
        len.payload[0] = 0xAA;
        CHECK(!mt_diag_nv(&P, &len, &rsp));
        req.payload[0] = 0x21; req.payload[1] = 0x00; req.payload[2] = 0;           /* a real NV item */
        CHECK(!mt_diag_nv(&P, &req, &rsp));
        npi_frame wr = {.cmd0 = 0x21, .cmd1 = 0x09, .length = 5};
        wr.payload[0] = 0xA0; wr.payload[1] = 0x0F;
        CHECK(!mt_diag_nv(&P, &wr, &rsp));
        npi_frame other = {.cmd0 = 0x24, .cmd1 = 0x08, .length = 3};                /* AF subsystem */
        other.payload[0] = 0xA0; other.payload[1] = 0x0F;
        CHECK(!mt_diag_nv(&P, &other, &rsp));
    }
    PASS("mt");
    return 0;
}
