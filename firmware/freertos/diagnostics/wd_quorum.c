/* SPDX-License-Identifier: MIT */
#include "wd_quorum.h"

void wd_quorum_init(wd_quorum_t *q) { *q = (wd_quorum_t){0}; }

bool wd_quorum_register(wd_quorum_t *q, unsigned bit) {
    if (bit >= 32 || (q->registered & (1u << bit))) return false;
    q->registered |= 1u << bit;
    return true;
}

void wd_quorum_checkin(wd_quorum_t *q, unsigned bit) {
    if (bit < 32) __atomic_fetch_or(&q->seen, 1u << bit, __ATOMIC_RELAXED);
}

bool wd_quorum_evaluate(wd_quorum_t *q) {
    uint32_t seen = __atomic_exchange_n(&q->seen, 0u, __ATOMIC_RELAXED);
    uint32_t missing = q->registered & ~seen;
    if (q->registered && !missing) {
        q->feeds++;
        q->consecutive_misses = 0;
        return true;
    }
    q->misses++;
    q->last_missing = missing;
    if (++q->consecutive_misses > q->worst_consecutive) q->worst_consecutive = q->consecutive_misses;
    return false;
}
