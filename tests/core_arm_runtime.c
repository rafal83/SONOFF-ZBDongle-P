/* SPDX-License-Identifier: MIT
 * Freestanding test image only: no vectors, CCFG, RF driver or flash routine.
 */
#include "core_indirect.h"
#include "core_broadcast.h"
#include "core_security.h"
core_indirect_t core_test_indirect;
core_broadcast_t core_test_broadcast;
core_security_t core_test_security;
void core_test_entry(void) { for (;;) {} }
bool core_test_ccm(void *ctx, bool encrypt, const uint8_t *key, const uint8_t *nonce,
    const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len, uint8_t *out, uint8_t *mic) {
    (void)ctx; (void)encrypt; (void)key; (void)nonce; (void)aad; (void)aad_len;
    (void)in; (void)len; (void)out; (void)mic; return false;
}
bool core_test_reserve(void *ctx, uint32_t upper) { (void)ctx; (void)upper; return false; }
void *memcpy(void *to, const void *from, size_t n) {
    uint8_t *d=to; const uint8_t *s=from;
    for (size_t i=0;i<n;i++) d[i]=s[i]; return to;
}
void *memset(void *to, int value, size_t n) {
    uint8_t *d=to; for (size_t i=0;i<n;i++) d[i]=(uint8_t)value; return to;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x=a, *y=b;
    for (size_t i=0;i<n;i++) if (x[i]!=y[i]) return x[i]<y[i]?-1:1;
    return 0;
}
void __aeabi_memclr(void *p, size_t n) { memset(p,0,n); }
void __aeabi_memclr4(void *p, size_t n) { memset(p,0,n); }
void __aeabi_memclr8(void *p, size_t n) { memset(p,0,n); }
void __aeabi_memcpy(void *to, const void *from, size_t n) { memcpy(to,from,n); }
void __aeabi_memcpy4(void *to, const void *from, size_t n) { memcpy(to,from,n); }
