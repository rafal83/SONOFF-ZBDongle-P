/* SPDX-License-Identifier: MIT; minimal freestanding C memory routines. */
#include <stddef.h>
void *memset(void *dst, int value, size_t length) {
    unsigned char *p = dst;
    for (size_t i = 0; i < length; i++) p[i] = (unsigned char)value;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t length) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < length; i++) d[i] = s[i];
    return dst;
}
