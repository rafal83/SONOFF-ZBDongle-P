/* SPDX-License-Identifier: MIT */
#ifndef FRT_HAL_WDT_H
#define FRT_HAL_WDT_H
#include <stdint.h>
void hal_wdt_start(uint32_t timeout_ms);
void hal_wdt_feed(void);
/* Remember which quorum members were last missing, for the expiry record. */
void hal_wdt_note_missing(uint32_t mask);
#endif
