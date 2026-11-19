/* SPDX-License-Identifier: MIT
 * Execute the actual generated TI function bodies against fault-injected NV.
 * Host fixture layouts do not constitute an ARM ABI or radio qualification.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PORT_EXPERIMENTAL_NEXT 1
#ifndef TEST_SECURITY_CAPACITY
#define TEST_SECURITY_CAPACITY 3
#endif
#define SUCCESS 0
#define ZSUCCESS SUCCESS
#define INVALID_NODE_ADDR UINT16_MAX
#define SEC_NO_KEY_NV_ID UINT16_MAX
#define ZCD_NV_APS_LINK_KEY_TABLE 1
#define ZCD_NV_EX_APS_KEY_DATA_TABLE 2
#define ZCD_NV_TRUSTCENTER_ADDR 3
#define Z_EXTADDR_LEN 8
#define MAX_APS_FRAMECOUNTER_CHANGES 1000u
typedef struct { uint16_t ami, keyNvId; uint32_t authenticateOption; } ZDSecMgrEntry_t;
typedef struct { uint16_t numRecs; } nvDeviceListHdr_t;
typedef struct { uint8_t key[16]; uint32_t txFrmCntr, rxFrmCntr; } APSME_ApsLinkKeyNVEntry_t;
typedef struct { uint32_t txFrmCntr, rxFrmCntr; } ram_key;
static const uint16_t gZDSECMGR_ENTRY_MAX = TEST_SECURITY_CAPACITY;
static ZDSecMgrEntry_t entries[TEST_SECURITY_CAPACITY];
static ZDSecMgrEntry_t *ZDSecMgrEntries = entries;
static ram_key ApsLinkKeyRAMEntry[TEST_SECURITY_CAPACITY + 1];
static APSME_ApsLinkKeyNVEntry_t keys[TEST_SECURITY_CAPACITY];
static uint8_t table[sizeof(nvDeviceListHdr_t)+sizeof(entries)], zgApsTrustCenterAddr[8];
static unsigned reads, writes, key_writes, allocations;
static bool fail_alloc, fail_read, fail_write;
static int fail_row, fail_key_read, fail_key_write;
static void *OsalPort_malloc(size_t n) {
    if(fail_alloc) return NULL;
    allocations++; void *p=malloc(n); assert(p);
    /* Deterministic old heap contents expose the native unwritten header. */
    memset(p,0,n); return p;
}
static void OsalPort_free(void *p) { assert(allocations); allocations--; free(p); }
#define OsalPort_memcpy memcpy
static unsigned osal_nv_read(unsigned id, unsigned offset, unsigned length, void *buffer) {
    if (id == ZCD_NV_TRUSTCENTER_ADDR) { memset(buffer,0,length); return SUCCESS; }
    assert(id==ZCD_NV_APS_LINK_KEY_TABLE && offset+length<=sizeof(table)); reads++;
    if (fail_read || (fail_row>=0 && offset==sizeof(nvDeviceListHdr_t)+(unsigned)fail_row*sizeof(ZDSecMgrEntry_t))) return 1;
    memcpy(buffer,table+offset,length); return SUCCESS;
}
static unsigned osal_nv_write(unsigned id, unsigned length, const void *buffer) {
    assert(id==ZCD_NV_APS_LINK_KEY_TABLE && (length==sizeof(table) || length==sizeof(nvDeviceListHdr_t))); writes++;
    if(fail_write) return 1;
    memcpy(table,buffer,length); return SUCCESS;
}
static unsigned osal_nv_read_ex(unsigned id, unsigned index, unsigned offset, unsigned length, void *buffer) {
    assert(id==ZCD_NV_EX_APS_KEY_DATA_TABLE && index<TEST_SECURITY_CAPACITY && offset==0 && length==sizeof(keys[0]));
    if ((int)index==fail_key_read) return 1;
    memcpy(buffer,&keys[index],length); return SUCCESS;
}
static unsigned osal_nv_write_ex(unsigned id, unsigned index, unsigned length, const void *buffer) {
    assert(id==ZCD_NV_EX_APS_KEY_DATA_TABLE && index<TEST_SECURITY_CAPACITY && length==sizeof(keys[0]));
    key_writes++;
    if ((int)index==fail_key_write) return 1;
    memcpy(&keys[index],buffer,length); return SUCCESS;
}
#undef PORT_EXPERIMENTAL_NEXT
#define ZDSecMgrUpdateNV LegacyUpdateNV
#define ZDSecMgrRestoreFromNV LegacyRestoreFromNV
#include "security_under_test.inc"
#undef ZDSecMgrUpdateNV
#undef ZDSecMgrRestoreFromNV
#define PORT_EXPERIMENTAL_NEXT 1
#include "security_under_test.inc"
static nvDeviceListHdr_t header(void) { nvDeviceListHdr_t h; memcpy(&h,table,sizeof(h)); return h; }
static void save(void) { nvDeviceListHdr_t h={0}; for(unsigned i=0;i<TEST_SECURITY_CAPACITY;i++) h.numRecs+=entries[i].ami!=INVALID_NODE_ADDR; memcpy(table,&h,sizeof(h)); memcpy(table+sizeof(h),entries,sizeof(entries)); }
static void reset(void) {
    assert(!allocations); reads=writes=key_writes=0; fail_alloc=fail_read=fail_write=false;
    fail_row=fail_key_read=fail_key_write=-1;
    memset(keys,0,sizeof(keys)); memset(ApsLinkKeyRAMEntry,0xa5,sizeof(ApsLinkKeyRAMEntry));
    for(unsigned i=0;i<TEST_SECURITY_CAPACITY;i++) entries[i]=(ZDSecMgrEntry_t){INVALID_NODE_ADDR,SEC_NO_KEY_NV_ID,0};
    save();
}
static void update_tests(void) {
    reset(); uint8_t before[sizeof(table)]; memcpy(before,table,sizeof(table));
    entries[0]=(ZDSecMgrEntry_t){42,0,1}; ZDSecMgrUpdateNV(0);
    assert(reads==1 && writes==1 && header().numRecs==1 && !allocations);
    for(unsigned i=0;i<500;i++) { entries[0].authenticateOption=i; ZDSecMgrUpdateNV(0); assert(header().numRecs==1); }
    assert(!memcmp(table+sizeof(nvDeviceListHdr_t)+sizeof(entries[0]),before+sizeof(nvDeviceListHdr_t)+sizeof(entries[0]),sizeof(entries)-sizeof(entries[0])));
    entries[0].ami=INVALID_NODE_ADDR;
    for(unsigned i=0;i<500;i++) { ZDSecMgrUpdateNV(0); assert(header().numRecs==0); }
    unsigned old_writes=writes; ZDSecMgrUpdateNV(TEST_SECURITY_CAPACITY); assert(writes==old_writes);
    for(unsigned failure=0;failure<3;failure++) {
        reset(); memcpy(before,table,sizeof(table)); entries[0].ami=42;
        fail_alloc=failure==0; fail_read=failure==1; fail_write=failure==2;
        ZDSecMgrUpdateNV(0); assert(!memcmp(table,before,sizeof(table)) && !allocations);
    }
}
static void commissioned(void) {
    reset();
    for(unsigned i=0;i<TEST_SECURITY_CAPACITY;i++) {
        entries[i]=(ZDSecMgrEntry_t){(uint16_t)(i+1),(uint16_t)i,1};
        keys[i].txFrmCntr=100; keys[i].rxFrmCntr=50; memset(keys[i].key,0x5a,16);
    }
    save();
}
static void restore_tests(void) {
    commissioned(); ZDSecMgrRestoreFromNV();
    assert(key_writes==TEST_SECURITY_CAPACITY && !allocations);
    for(unsigned i=0;i<TEST_SECURITY_CAPACITY;i++) {
        assert(entries[i].ami==i+1 && ApsLinkKeyRAMEntry[i].txFrmCntr==1101 && ApsLinkKeyRAMEntry[i].rxFrmCntr==50);
        assert(keys[i].txFrmCntr==1101 && keys[i].key[0]==0x5a);
    }
    for(unsigned failure=0;failure<9;failure++) {
        commissioned();
        if(failure==0) fail_alloc=true;
        if(failure==1) fail_read=true;
        if(failure==2) fail_row=0;
        if(failure==3) fail_key_read=0;
        if(failure==4) fail_key_write=0;
        if(failure==5) { entries[0].keyNvId=UINT16_MAX; save(); }
        if(failure==6) keys[0].txFrmCntr=UINT32_MAX-1000;
        if(failure==7) keys[0].txFrmCntr=UINT32_MAX-1001;
        if(failure==8) { nvDeviceListHdr_t invalid={UINT16_MAX}; memcpy(table,&invalid,sizeof(invalid)); }
        ram_key initial=ApsLinkKeyRAMEntry[0], canary=ApsLinkKeyRAMEntry[TEST_SECURITY_CAPACITY];
        ZDSecMgrRestoreFromNV();
        assert(entries[0].ami==INVALID_NODE_ADDR && entries[0].keyNvId==SEC_NO_KEY_NV_ID);
        assert(!memcmp(&initial,&ApsLinkKeyRAMEntry[0],sizeof(initial)) && !allocations);
        assert(!memcmp(&canary,&ApsLinkKeyRAMEntry[TEST_SECURITY_CAPACITY],sizeof(canary)));
    }
}
static void baseline_tests(void) {
    reset(); entries[0]=(ZDSecMgrEntry_t){42,0,1}; LegacyUpdateNV(0);
    assert(reads==TEST_SECURITY_CAPACITY+1 && writes==2 && header().numRecs==1);
    entries[1]=(ZDSecMgrEntry_t){43,1,1}; save(); assert(header().numRecs==2);
    LegacyUpdateNV(0); assert(header().numRecs==1); /* Native buffer header was never populated. */
    commissioned(); LegacyRestoreFromNV();
    APSME_ApsLinkKeyNVEntry_t saved_keys[TEST_SECURITY_CAPACITY];
    ram_key saved_ram[TEST_SECURITY_CAPACITY+1];
    memcpy(saved_keys,keys,sizeof(keys)); memcpy(saved_ram,ApsLinkKeyRAMEntry,sizeof(saved_ram));
    commissioned(); ZDSecMgrRestoreFromNV();
    assert(!memcmp(saved_keys,keys,sizeof(keys)) && !memcmp(saved_ram,ApsLinkKeyRAMEntry,sizeof(saved_ram)));
}
int main(void) { update_tests(); restore_tests(); baseline_tests(); printf("Security persistence: %u slots, 1000 idempotent transitions, single-record commit, nine restore failure classes, native-success differential passed; NV API calls %u reads/2 writes -> 1 read/1 write\n", TEST_SECURITY_CAPACITY, TEST_SECURITY_CAPACITY+1); return 0; }
