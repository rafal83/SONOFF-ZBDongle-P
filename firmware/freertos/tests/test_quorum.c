/* SPDX-License-Identifier: MIT */
#include "frt_test.h"
#include "wd_quorum.h"

int main(void) {
    wd_quorum_t q;
    wd_quorum_init(&q);
    CHECK(!wd_quorum_evaluate(&q));           /* nobody registered: never feed */
    CHECK(q.misses == 1);
    wd_quorum_init(&q);
    CHECK(wd_quorum_register(&q, 0));
    CHECK(wd_quorum_register(&q, 2));
    CHECK(!wd_quorum_register(&q, 2));
    CHECK(!wd_quorum_register(&q, 32));
    wd_quorum_checkin(&q, 0);
    CHECK(!wd_quorum_evaluate(&q));
    CHECK(q.last_missing == 4u && q.consecutive_misses == 1);
    wd_quorum_checkin(&q, 0);
    wd_quorum_checkin(&q, 2);
    wd_quorum_checkin(&q, 5);                 /* unregistered bit is harmless */
    CHECK(wd_quorum_evaluate(&q));
    CHECK(q.feeds == 1 && q.consecutive_misses == 0);
    CHECK(!wd_quorum_evaluate(&q));           /* check-ins are consumed */
    CHECK(!wd_quorum_evaluate(&q));
    CHECK(q.worst_consecutive == 2 && q.last_missing == 5u);
    PASS("wd_quorum");
    return 0;
}
