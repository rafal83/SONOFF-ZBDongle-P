/* SPDX-License-Identifier: MIT */
#include "core_security.h"
#include <string.h>
static void clear(void *p, size_t n) { volatile uint8_t *v=p; while (n--) *v++=0; }
typedef struct __attribute__((packed)) { uint32_t lo, hi; } u64_packed_t;
typedef struct __attribute__((packed)) { uint32_t v; } u32_packed_t;
static inline uint64_t address_key(const uint8_t p[8]) {
    const u64_packed_t *u = (const u64_packed_t *)(const void *)p;
    return (uint64_t)u->lo | ((uint64_t)u->hi << 32u);
}
static inline uint32_t read32(const uint8_t p[4]) {
    return ((const u32_packed_t *)(const void *)p)->v;
}
static inline void write32(uint8_t p[4], uint32_t n) {
    ((u32_packed_t *)(void *)p)->v = n;
}
static int key_slot(core_security_t *s, uint8_t sequence) {
    for (unsigned i=0;i<2;i++) if (s->network_keys[i].valid && s->network_keys[i].sequence==sequence) return (int)i;
    return -1;
}
/* The adapter supplies NWK header length. Independently verify it before
 * interpreting auxiliary data, including optional IEEE and source routes. */
static bool header_valid(const uint8_t *frame, size_t available, size_t header) {
    if (!frame || header<8 || header>available || (frame[0]&0x3cu)!=8 || (frame[0]&3u)>1) return false;
    size_t expected=8; uint8_t options=frame[1];
    if (options&0x08) expected+=8;
    if (options&0x10) expected+=8;
    if (options&0x01) expected++;
    if (expected>header) return false;
    if (options&0x04) {
        if (header-expected<2 || !frame[expected] || frame[expected+1]>=frame[expected]) return false;
        expected+=2u+2u*frame[expected];
    }
    return expected==header;
}
void core_security_init(core_security_t *s, uint32_t upper,
    core_security_ccm_t ccm, core_security_reserve_t reserve, void *context) {
    memset(s,0,sizeof(*s));
    core_index_init(&s->peers,s->branches,s->addresses,s->occupied,CORE_SECURITY_PEERS);
    for (uint16_t i=0;i<CORE_SECURITY_PEERS;i++) s->free_next[i]=i+1u<CORE_SECURITY_PEERS?(uint16_t)(i+1u):CORE_INDEX_NONE;
    s->tx_next=s->tx_reserved=upper; s->ccm=ccm; s->reserve=reserve; s->context=context;
}
void core_security_clear(core_security_t *s) { clear(s,sizeof(*s)); }
bool core_security_set_key(core_security_t *s, unsigned slot, uint8_t sequence, const uint8_t key[16]) {
    if (slot>=2 || !key) return false;
    core_security_key_t *k=&s->network_keys[slot];
    if (k->valid && k->sequence==sequence && !memcmp(k->key,key,16)) return true;
    if (s->network_keys[1u-slot].valid && s->network_keys[1u-slot].sequence==sequence) return false;
    if (k->epoch==UINT16_MAX) return false;
    clear(k->key,16); memcpy(k->key,key,16); k->sequence=sequence; k->valid=true; k->epoch++;
    /* No scan of peer rows. Epoch comparison invalidates old replay contexts
     * lazily, after authentication, and never resets outgoing counters. */
    return true;
}
bool core_security_register(core_security_t *s, const uint8_t address[8]) {
    if (!address) return false;
    uint64_t key=address_key(address);
    if (core_index_find(&s->peers,key)!=CORE_INDEX_NONE) return true;
    if (s->free_head==CORE_INDEX_NONE) return false;
    uint16_t slot=s->free_head;
    if (!core_index_insert(&s->peers,key,slot)) return false;
    s->free_head=s->free_next[slot]; s->replay[slot]=(core_security_peer_t){0}; return true;
}
bool core_security_unregister(core_security_t *s, const uint8_t address[8]) {
    if (!address) return false;
    uint16_t slot=core_index_remove(&s->peers,address_key(address));
    if (slot==CORE_INDEX_NONE) return false;
    clear(&s->replay[slot],sizeof(s->replay[slot]));
    s->free_next[slot]=s->free_head; s->free_head=slot; return true;
}
bool core_security_restore_peer_counter(core_security_t *s, const uint8_t address[8], uint8_t sequence, uint32_t counter) {
    if (!address || counter==UINT32_MAX) return false;
    int k=key_slot(s,sequence);
    uint16_t peer=core_index_find(&s->peers,address_key(address));
    if (k<0 || peer==CORE_INDEX_NONE) return false;
    core_security_peer_t *r=&s->replay[peer];
    if (r->epoch[k]==s->network_keys[k].epoch && counter<r->counter[k]) return false;
    r->counter[k]=counter; r->epoch[k]=s->network_keys[k].epoch; return true;
}
bool core_security_peer_counter(core_security_t *s, const uint8_t address[8], uint8_t sequence, uint32_t *counter) {
    int k=key_slot(s,sequence);
    uint16_t peer=address?core_index_find(&s->peers,address_key(address)):CORE_INDEX_NONE;
    if (k<0 || peer==CORE_INDEX_NONE || s->replay[peer].epoch[k]!=s->network_keys[k].epoch) return false;
    *counter=s->replay[peer].counter[k]; return true;
}
bool core_security_seal(core_security_t *s, unsigned k, const uint8_t local[8], uint8_t *frame,
    size_t header, size_t payload, size_t capacity, size_t *length) {
    if (length) *length=0;
    if (k>=2 || !s->network_keys[k].valid || !s->ccm || !s->reserve || !local || !payload ||
        header>CORE_SECURITY_MAX_FRAME-18u || payload>CORE_SECURITY_MAX_FRAME-18u-header ||
        capacity<header+payload+18u || !header_valid(frame,header+payload,header)) return false;
    if (s->tx_next==UINT32_MAX) return false;
    if (s->tx_next==s->tx_reserved) {
        uint32_t room=UINT32_MAX-s->tx_next;
        uint32_t step=room<CORE_SECURITY_RESERVATION?room:CORE_SECURITY_RESERVATION;
        uint32_t upper=s->tx_next+step;
        if (!step || !s->reserve(s->context,upper)) return false;
        s->tx_reserved=upper;
    }
    uint32_t counter=s->tx_next++; /* Failed crypto still consumes its nonce. */
    uint8_t aad[CORE_SECURITY_MAX_FRAME], out[CORE_SECURITY_MAX_FRAME], nonce[13], mic[4];
    memcpy(aad,frame,header); aad[1]|=2;
    uint8_t *aux=aad+header; aux[0]=0x2d;
    write32(aux+1,counter); memcpy(aux+5,local,8); aux[13]=s->network_keys[k].sequence;
    memcpy(nonce,local,8); write32(nonce+8,counter); nonce[12]=0x2d;
    bool ok=s->ccm(s->context,true,s->network_keys[k].key,nonce,aad,header+14,
        frame+header,payload,out,mic);
    if (ok) {
        aux[0]=0x28; memcpy(frame,aad,header+14); memcpy(frame+header+14,out,payload);
        memcpy(frame+header+14+payload,mic,4); if (length) *length=header+14+payload+4;
    }
    clear(out,sizeof(out)); clear(mic,sizeof(mic)); return ok;
}
static bool open_frame(core_security_t *s, uint8_t *frame, size_t length, size_t header, size_t *payload_len, bool admit) {
    if (payload_len) *payload_len=0;
    if (!s->ccm || length>CORE_SECURITY_MAX_FRAME || header>length || length-header<19 ||
        !header_valid(frame,length,header) || !(frame[1]&2)) return false;
    uint8_t *aux=frame+header;
    /* This implementation deliberately handles only NWK ENC-MIC-32 with
     * an extended nonce and the mandated zero on-wire level. */
    if (aux[0]!=0x28) return false;
    int k=key_slot(s,aux[13]); if (k<0) return false;
    uint16_t peer=core_index_find(&s->peers,address_key(aux+5));
    if (peer==CORE_INDEX_NONE && !admit) return false;
    core_security_peer_t *r=peer==CORE_INDEX_NONE?NULL:&s->replay[peer]; uint32_t counter=read32(aux+1);
    if (counter==UINT32_MAX || (r && r->epoch[k]==s->network_keys[k].epoch && counter<=r->counter[k])) return false;
    uint8_t aad[CORE_SECURITY_MAX_FRAME], out[CORE_SECURITY_MAX_FRAME], nonce[13], mic[4];
    size_t payload=length-header-18;
    memcpy(aad,frame,header+14); aad[header]=0x2d;
    memcpy(nonce,aux+5,8); memcpy(nonce+8,aux+1,4); nonce[12]=0x2d;
    memcpy(mic,frame+length-4,4);
    bool ok=s->ccm(s->context,false,s->network_keys[k].key,nonce,aad,header+14,
        aux+14,payload,out,mic);
    if (ok && !r) {
        ok=core_security_register(s,aux+5);
        if (ok) { peer=core_index_find(&s->peers,address_key(aux+5)); r=&s->replay[peer]; }
    }
    if (ok) {
        /* Only authentication success publishes plaintext or replay state. */
        memcpy(aux+14,out,payload); r->counter[k]=counter; r->epoch[k]=s->network_keys[k].epoch;
        if (payload_len) *payload_len=payload;
    }
    clear(out,sizeof(out)); clear(mic,sizeof(mic)); return ok;
}
bool core_security_open(core_security_t *s, uint8_t *frame, size_t length, size_t header, size_t *payload_len) {
    return open_frame(s,frame,length,header,payload_len,false);
}
bool core_security_open_authenticated_peer(core_security_t *s, uint8_t *frame, size_t length, size_t header, size_t *payload_len) {
    return open_frame(s,frame,length,header,payload_len,true);
}
