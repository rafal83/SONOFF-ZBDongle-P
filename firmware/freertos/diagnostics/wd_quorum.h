/* SPDX-License-Identifier: MIT
 * Watchdog quorum: the hardware watchdog is fed only when every registered
 * task checked in since the previous evaluation. Check-ins are atomic ORs and
 * may come from any task; evaluate() is called by the supervisor only.
 */
#ifndef FRT_WD_QUORUM_H
#define FRT_WD_QUORUM_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t registered;
    volatile uint32_t seen;
    uint32_t feeds, misses, last_missing, consecutive_misses, worst_consecutive;
} wd_quorum_t;

void wd_quorum_init(wd_quorum_t *q);
bool wd_quorum_register(wd_quorum_t *q, unsigned bit);
void wd_quorum_checkin(wd_quorum_t *q, unsigned bit);
/* true: everyone checked in -> caller feeds the watchdog. */
bool wd_quorum_evaluate(wd_quorum_t *q);
#endif
