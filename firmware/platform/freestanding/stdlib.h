/* SPDX-License-Identifier: MIT
 * Pinned TI osc.c includes stdlib.h only for NULL, supplied by stddef.h.
 * This diagnostic has no libc/allocator. Any future use must fail compilation.
 */
#ifndef PORT_UNUSED_STDLIB_H
#define PORT_UNUSED_STDLIB_H
#include <stddef.h>
#endif
