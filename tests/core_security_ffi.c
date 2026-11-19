/* SPDX-License-Identifier: MIT
 * Host test ABI only. Export wrappers rather than changing firmware symbols.
 */
#include "core_security.h"
#include <string.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
EXPORT size_t test_security_size(void) { return sizeof(core_security_t); }
EXPORT void test_security_init(core_security_t *s, uint32_t upper,
    core_security_ccm_t crypto, core_security_reserve_t reserve, void *ctx) {
    core_security_init(s,upper,crypto,reserve,ctx);
}
EXPORT bool test_security_key(core_security_t *s, unsigned slot, uint8_t seq, const uint8_t *key) {
    return core_security_set_key(s,slot,seq,key);
}
EXPORT bool test_security_peer(core_security_t *s, const uint8_t *address) { return core_security_register(s,address); }
EXPORT bool test_security_delete(core_security_t *s, const uint8_t *address) { return core_security_unregister(s,address); }
EXPORT bool test_security_restore(core_security_t *s, const uint8_t *address, uint8_t seq, uint32_t counter) {
    return core_security_restore_peer_counter(s,address,seq,counter);
}
EXPORT bool test_security_seal(core_security_t *s, unsigned slot, const uint8_t *address,
    uint8_t *frame, size_t header, size_t payload, size_t capacity, size_t *length) {
    return core_security_seal(s,slot,address,frame,header,payload,capacity,length);
}
EXPORT bool test_security_open(core_security_t *s, uint8_t *frame, size_t length, size_t header, size_t *payload) {
    return core_security_open(s,frame,length,header,payload);
}
EXPORT uint32_t test_security_tx_next(core_security_t *s) { return s->tx_next; }
EXPORT uint32_t test_security_counter(core_security_t *s, const uint8_t *address, unsigned key_slot) {
    uint64_t key=0; for (unsigned i=0;i<8;i++) key|=(uint64_t)address[i]<<(8*i);
    uint16_t slot=core_index_find(&s->peers,key);
    return slot==CORE_INDEX_NONE || key_slot>=2?UINT32_MAX:s->replay[slot].counter[key_slot];
}
EXPORT bool test_security_clear(core_security_t *s) {
    core_security_clear(s);
    const unsigned char *p=(const unsigned char *)s;
    for (size_t i=0;i<sizeof(*s);i++) if (p[i]) return false;
    return true;
}
