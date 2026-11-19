/* SPDX-License-Identifier: MIT
 * Distinguish this experimental optimized build from the upstream baseline.
 */
#undef CODE_REVISION_NUMBER
#if defined(PORT_EXPERIMENTAL_CORES) && defined(PORT_SDK_833)
#define CODE_REVISION_NUMBER 2026111802
#elif defined(PORT_EXPERIMENTAL_CORES)
#define CODE_REVISION_NUMBER 2026111701
#elif defined(PORT_EXPERIMENTAL_NEXT)
#define CODE_REVISION_NUMBER 20261117
#else
#define CODE_REVISION_NUMBER 20261114
#endif
#define PORT_ZNP_OPTIMIZED 1
/* The Koenkk board header is retained; this complements its driver indices.
 * The optimized SysConfig input creates exactly one watchdog instance. */
#define CONFIG_PORT_WATCHDOG 0
/* Native TI NWK limits: leave at least 48 of the 72 buffer slots outside the
 * global indirect hold quota. This reserves slots, not physical heap bytes. */
#define PORT_INDIRECT_MAX_PER_CHILD 3
#define PORT_INDIRECT_MAX_ALL 24
/* Capacity adjustment only; not a claim of O(1) broadcast processing. */
#undef MAX_BCAST
#define MAX_BCAST 40
