/* SPDX-License-Identifier: MIT */
#include "mt.h"

static void rpc_error(npi_frame *rsp, uint8_t code, const npi_frame *req) {
    rsp->cmd0 = MT_TYPE_SRSP | MT_SUBSYS_RPC_ERR;
    rsp->cmd1 = 0;
    rsp->length = 3;
    rsp->payload[0] = code;
    rsp->payload[1] = req->cmd0;
    rsp->payload[2] = req->cmd1;
}

static bool sys_request(const mt_platform_t *p, const npi_frame *req, npi_frame *rsp) {
    uint8_t type = req->cmd0 & 0xE0u;
    if (req->cmd1 == MT_SYS_RESET_REQ && type == MT_TYPE_AREQ) {
        /* One byte (0 hard, 1 soft); both perform a full MCU reset here. */
        if (req->length == 1) p->reset(p->context, req->payload[0]);
        return false;
    }
    if (type != MT_TYPE_SREQ) return false; /* unknown AREQ: ignored */
    rsp->cmd0 = MT_TYPE_SRSP | MT_SUBSYS_SYS;
    rsp->cmd1 = req->cmd1;
    switch (req->cmd1) {
    case MT_SYS_PING:
        if (req->length) break;
        rsp->length = 2;
        rsp->payload[0] = (uint8_t)MT_CAP_SYS;
        rsp->payload[1] = (uint8_t)(MT_CAP_SYS >> 8);
        return true;
    case MT_SYS_VERSION:
        if (req->length) break;
        rsp->length = 9;
        rsp->payload[0] = FRT_TRANSPORT_REV;
        rsp->payload[1] = FRT_PRODUCT_ID;
        rsp->payload[2] = FRT_VERSION_MAJOR;
        rsp->payload[3] = FRT_VERSION_MINOR;
        rsp->payload[4] = FRT_VERSION_PATCH;
        for (unsigned i = 0; i < 4; i++) rsp->payload[5 + i] = (uint8_t)(p->build_revision >> (8u * i));
        return true;
    case MT_SYS_GET_EXTADDR:
        if (req->length) break;
        rsp->length = 8;
        p->ieee(p->context, rsp->payload);
        return true;
    default:
        rpc_error(rsp, MT_RPC_ERR_COMMAND_ID, req);
        return true;
    }
    rpc_error(rsp, MT_RPC_ERR_LENGTH, req);
    return true;
}

bool mt_dispatch(const mt_platform_t *p, const npi_frame *req, npi_frame *rsp) {
    uint8_t type = req->cmd0 & 0xE0u;
    uint8_t subsystem = req->cmd0 & 0x1Fu;
    if (type != MT_TYPE_SREQ && type != MT_TYPE_AREQ) return false;
    if (subsystem == MT_SUBSYS_SYS) return sys_request(p, req, rsp);
    if (type != MT_TYPE_SREQ) return false;
    if (subsystem != MT_SUBSYS_FRT_DIAG) {
        rpc_error(rsp, MT_RPC_ERR_SUBSYSTEM, req);
        return true;
    }
    int n = p->diag(p->context, req->cmd1, req->payload, req->length, rsp->payload, NPI_MAX_PAYLOAD);
    if (n == MT_DIAG_UNKNOWN) { rpc_error(rsp, MT_RPC_ERR_COMMAND_ID, req); return true; }
    if (n < 0 || n > (int)NPI_MAX_PAYLOAD) { rpc_error(rsp, MT_RPC_ERR_PARAMETER, req); return true; }
    rsp->cmd0 = MT_TYPE_SRSP | MT_SUBSYS_FRT_DIAG;
    rsp->cmd1 = req->cmd1;
    rsp->length = (uint8_t)n;
    return true;
}

void mt_build_reset_ind(npi_frame *out, uint8_t reason) {
    out->cmd0 = MT_TYPE_AREQ | MT_SUBSYS_SYS;
    out->cmd1 = MT_SYS_RESET_IND;
    out->length = 6;
    out->payload[0] = reason;
    out->payload[1] = FRT_TRANSPORT_REV;
    out->payload[2] = FRT_PRODUCT_ID;
    out->payload[3] = FRT_VERSION_MAJOR;
    out->payload[4] = FRT_VERSION_MINOR;
    out->payload[5] = FRT_VERSION_PATCH;
}

uint8_t mt_reset_reason(uint32_t rstsrc, bool watchdog_record) {
    /* CC26x2 RSTSRC (AON_PMCTL RESETCTL.RESET_SRC >> 1): 0 power-on,
     * 1 pin, 2 VDDS loss, 4 VDDR loss, 5 clock loss, 6 SYSRESET, 7 warm
     * reset (CPU SYSRESETREQ, observed on hardware after SYS_RESET_REQ),
     * 8/9 wake-up from shutdown. A watchdog reset is also a warm reset; only
     * the crash record (when it survives) tells them apart. */
    if (watchdog_record) return 2;
    switch (rstsrc) {
    case 0: case 2: case 4: return 0;
    default: return 1;
    }
}
