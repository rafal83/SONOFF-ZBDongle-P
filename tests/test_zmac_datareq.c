/* Execute the TI source function with mock allocation, crypto and MAC APIs.
 * Types model the used fields; the ARM build checks the actual SDK ABI.
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define MAC_NO_RESOURCES 0x1au
#define ZSuccess 0
typedef struct { uint8_t bytes[12]; } ApiMac_sAddr_t;
typedef uint8_t ApiMac_addrType_t;
typedef struct {
    ApiMac_sAddr_t DstAddr;
    uint16_t DstPANId, TxOptions;
    uint8_t SrcAddrMode, Handle, Channel, Power, GpOffset, GpDuration, msduLength;
    uint8_t *msdu;
} ZMacDataReq_t;
typedef struct {
    ApiMac_sAddr_t dstAddr;
    uint16_t dstPanId, txOptions;
    uint8_t srcAddrMode, msduHandle, channel, power, gpOffset, gpDuration;
    struct { uint8_t len; uint8_t *p; } msdu;
} ApiMac_mcpsDataReq_t;
typedef uint8_t (*applySecCB_t)(uint8_t, uint8_t *);
static unsigned allocated, frees, sends, sec_calls;
static int reject_alloc, reject_sec;
static uint8_t mac_status;
static ZMacDataReq_t expected;
static void *OsalPort_malloc(size_t size) {
    if (reject_alloc || !size) return NULL;
    void *p = malloc(size); assert(p); allocated++; return p;
}
static void OsalPort_free(void *p) { assert(p && allocated); allocated--; frees++; free(p); }
#define OsalPort_memcpy memcpy
static void convertToTxOptions(uint16_t *out, uint16_t in) { *out = in; }
static uint8_t secure(uint8_t size, uint8_t *p) {
    assert(allocated && size == expected.msduLength && p != expected.msdu);
    sec_calls++;
    for (unsigned i = 0; i < size; i++) p[i] ^= 0xa5;
    return reject_sec ? 1 : ZSuccess;
}
static uint8_t MAP_MAC_McpsDataReq(ApiMac_mcpsDataReq_t *p) {
    assert(!memcmp(&p->dstAddr, &expected.DstAddr, sizeof(p->dstAddr)));
    assert(p->dstPanId == expected.DstPANId && p->srcAddrMode == expected.SrcAddrMode);
    assert(p->msduHandle == expected.Handle && p->txOptions == expected.TxOptions);
    assert(p->channel == expected.Channel && p->power == expected.Power);
    assert(p->gpOffset == expected.GpOffset && p->gpDuration == expected.GpDuration);
    assert(p->msdu.len == expected.msduLength);
    assert(!p->msdu.len || (p->msdu.p && allocated));
    for (unsigned i = 0; i < p->msdu.len; i++)
        assert(p->msdu.p[i] == (uint8_t)(expected.msdu[i] ^ (sec_calls ? 0xa5 : 0)));
    sends++; return mac_status;
}
#include "zmac_datareq_under_test.inc"
int main(void) {
    uint8_t payload[127], original[127];
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    memcpy(original, payload, sizeof(payload));
    expected = (ZMacDataReq_t){.DstPANId=0x1234, .SrcAddrMode=2, .Handle=17, .TxOptions=5,
        .Channel=15, .Power=20, .GpOffset=3, .GpDuration=8, .msduLength=sizeof(payload), .msdu=payload};
    reject_alloc = 1;
    assert(ZMacDataReqSec(&expected, secure) == MAC_NO_RESOURCES);
    assert(!sends && !sec_calls && !allocated && !frees);
    reject_alloc = 0;
    assert(ZMacDataReqSec(&expected, NULL) == ZSuccess);
    assert(sends == 1 && !allocated && frees == 1);
    assert(ZMacDataReqSec(&expected, secure) == ZSuccess);
    assert(sends == 2 && !allocated && frees == 2 && sec_calls == 1);
    reject_sec = 1;
    assert(ZMacDataReqSec(&expected, secure) == MAC_NO_RESOURCES);
    assert(sends == 2 && !allocated && frees == 3);
    reject_sec = 0; mac_status = 0xe1;
    assert(ZMacDataReqSec(&expected, secure) == mac_status);
    assert(sends == 3 && !allocated && frees == 4);
    assert(!memcmp(payload, original, sizeof(payload)));
    expected.msduLength = 0; sec_calls = 0;
    assert(ZMacDataReqSec(&expected, secure) == mac_status);
    assert(sends == 4 && !sec_calls && !allocated && frees == 4);
    puts("PASS: actual ZMacDataReqSec, nonempty allocation failure never reaches MAC, security failure cleanup, MAC failure cleanup, caller payload unchanged, empty MSDU");
    return 0;
}
