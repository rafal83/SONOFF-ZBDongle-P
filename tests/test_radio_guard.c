/* Bounds tests at an inaccessible page boundary, including valid NWK options. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "radio_guard.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
static uint32_t random_state = 0x18765ab;
static uint32_t random_word(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    return random_state ^= random_state << 5;
}
static uint8_t *page_end;
static unsigned checked;
static bool check(const uint8_t *frame, size_t size, unsigned level) {
    assert(size <= 127);
    uint8_t *guarded = page_end - size;
    if (size) memcpy(guarded, frame, size);
    bool result = port_radio_guard_accept(guarded, size, (uint8_t)level);
    assert(!size || !memcmp(frame, guarded, size));
    checked++; return result;
}
/* Model only fields touched by the generated callback prefix, not its ABI. */
#define MAC_MCPS_DATA_IND 13
typedef struct {
    struct { uint8_t event; } hdr;
    struct { struct { uint8_t *p; uint16_t len; } msdu; } dataInd;
} macCbackEvent_t;
const uint8_t zmacCBSizeTable[19] = {0};
static struct { uint8_t SecurityLevel; } _NIB = {5};
static unsigned released, forwarded;
static void MAP_mac_msg_deallocate(uint8_t **p) { assert(p && *p); *p = NULL; released++; }
static void callback_prefix(macCbackEvent_t *pData) {
#include "zmac_rx_guard_under_test.inc"
    forwarded++;
}
int main(void) {
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info);
    uint8_t *memory = VirtualAlloc(NULL, 2 * info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(memory);
    DWORD previous;
    assert(VirtualProtect(memory + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous));
    page_end = memory + info.dwPageSize;
#else
    uint8_t memory[4096]; page_end = memory + sizeof(memory);
#endif
    uint8_t p[127] = {0x08}; /* Protocol 2, data. */
    assert(!port_radio_guard_accept(NULL, 0, 5));
    assert(!port_radio_guard_accept(NULL, 127, 5));
    assert(!port_radio_guard_accept(p, 128, 5));
    for (unsigned i = 0; i < 9; i++) assert(!check(p, i, 5));
    assert(check(p, 9, 5));
    /* Every combination of optional NWK fields, all security levels. */
    for (unsigned flags = 0; flags < 32; flags++) {
        for (unsigned level = 0; level < 8; level++) {
            memset(p, 0, sizeof(p)); p[0] = 0x08; p[1] = (uint8_t)flags;
            size_t h = 8 + ((flags & 8) ? 8 : 0) + ((flags & 16) ? 8 : 0) + ((flags & 1) ? 1 : 0);
            size_t route = h;
            if (flags & 4) { p[h] = 3; p[h + 1] = 2; h += 8; }
            size_t aux = h;
            if (flags & 2) { p[h] = 0x28; h += 14 + ((level & 3) == 0 ? 0 : 2u << (level & 3)); }
            assert(check(p, h + 1, level));
            for (size_t cut = 0; cut <= h; cut++) assert(!check(p, cut, level));
            if (flags & 4) {
                p[route + 1] = 3; assert(!check(p, h + 1, level));
                p[route + 1] = 255; assert(!check(p, h + 1, level));
                p[route + 1] = 0; p[route] = 0; assert(!check(p, h + 1, level));
                p[route] = 255; assert(!check(p, h + 1, level)); p[route] = 3;
            }
            if (flags & 2) {
                p[aux] = 0x08; assert(!check(p, h + 1, level));
                p[aux] = 0x20; assert(!check(p, h + 1, level));
                p[aux] = 0x28; assert(!check(p, h + 1, 255));
                /* On-wire security level must not determine MIC bounds. */
                for (unsigned wire_level = 0; wire_level < 8; wire_level++) {
                    p[aux] = (uint8_t)(0x28 | wire_level); assert(check(p, h + 1, level));
                }
            }
        }
    }
    memset(p, 0, sizeof(p)); p[0] = 0x0b; p[2] = 3; /* Stub APS, unicast. */
    assert(!check(p, 2, 5)); assert(!check(p, 6, 5)); assert(check(p, 7, 5));
    p[2] = 15; assert(!check(p, 8, 5)); assert(check(p, 9, 5)); /* Group. */
    p[0] = 0x8c; assert(!check(p, 1, 5)); assert(check(p, 2, 5)); /* Native GP dispatch. */
    p[0] = 0x8e; assert(!check(p, 2, 5)); /* Reserved GP frame type. */
    p[0] = 0x8f; assert(!check(p, 2, 5)); /* Must not reach the Stub APS callback. */
    callback_prefix(NULL);
    macCbackEvent_t event = {.hdr.event = 255}; callback_prefix(&event);
    event.hdr.event = MAC_MCPS_DATA_IND; event.dataInd.msdu.p = page_end - 1;
    event.dataInd.msdu.len = 1; page_end[-1] = 0x08; callback_prefix(&event);
    assert(released == 1 && !forwarded);
    memset(p, 0, sizeof(p)); p[0] = 0x08;
    event.dataInd.msdu.p = p; event.dataInd.msdu.len = 9; callback_prefix(&event);
    assert(released == 1 && forwarded == 1);
    event.hdr.event = 12; event.dataInd.msdu.p = NULL; callback_prefix(&event);
    assert(released == 1 && forwarded == 2); /* Non-DATA_IND retains native ownership. */
    for (unsigned trial = 0; trial < 2000000; trial++) {
        size_t size = random_word() & 127u;
        for (size_t j = 0; j < size; j++) p[j] = (uint8_t)random_word();
        if ((trial & 1) && size) p[0] = (uint8_t)((p[0] & 0xc3) | 0x08);
        (void)check(p, size, random_word() & 7);
    }
#ifdef _WIN32
    assert(VirtualFree(memory, 0, MEM_RELEASE));
#endif
    printf("PASS: radio ingress bounds, all NWK header options/MIC levels/truncations, relay bounds, Stub APS/GP dispatch, callback ownership; %u page-boundary probes including 2000000 fuzz cases\n", checked);
    return 0;
}
