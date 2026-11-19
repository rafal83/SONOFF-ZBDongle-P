/* SPDX-License-Identifier: MIT
 * TI driverlib includes stdlib.h only for NULL. No allocator exists in this
 * target: any malloc/free use fails to link. */
#ifndef FRT_STDLIB_H
#define FRT_STDLIB_H
#include <stddef.h>
#endif
