/* SPDX-License-Identifier: MIT
 * Minimal C runtime for the freestanding FreeRTOS image. Byte loops are kept
 * simple and auditable; word-sized fast paths only for aligned blocks. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
        for (; n >= 4; n -= 4, d += 4, s += 4) *(uint32_t *)(void *)d = *(const uint32_t *)(const void *)s;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int value, size_t n) {
    uint8_t *d = dst;
    if (n >= 4 && ((uintptr_t)d & 3u) == 0) {
        uint32_t w = (uint8_t)value;
        w |= w << 8;
        w |= w << 16;
        for (; n >= 4; n -= 4, d += 4) *(uint32_t *)(void *)d = w;
    }
    while (n--) *d++ = (uint8_t)value;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) return *x < *y ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ARM EABI helpers that clang may emit for struct copies/clears. */
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) __attribute__((alias("__aeabi_memcpy")));
void __aeabi_memcpy8(void *d, const void *s, size_t n) __attribute__((alias("__aeabi_memcpy")));
void __aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove4(void *d, const void *s, size_t n) __attribute__((alias("__aeabi_memmove")));
void __aeabi_memmove8(void *d, const void *s, size_t n) __attribute__((alias("__aeabi_memmove")));
void __aeabi_memset(void *d, size_t n, int v) { memset(d, v, n); }
void __aeabi_memset4(void *d, size_t n, int v) __attribute__((alias("__aeabi_memset")));
void __aeabi_memset8(void *d, size_t n, int v) __attribute__((alias("__aeabi_memset")));
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) __attribute__((alias("__aeabi_memclr")));
void __aeabi_memclr8(void *d, size_t n) __attribute__((alias("__aeabi_memclr")));
