/* SPDX-License-Identifier: MIT
 * Supervisor: lowest application priority on purpose. If any higher
 * priority task monopolises the CPU, the supervisor itself misses its
 * check-in and the watchdog fires.
 */
#include "app.h"
#include "hal_wdt.h"

app_cpu_stats_t app_cpu;

static StaticTask_t tcb;
static StackType_t stack[STACK_SUPERVISOR];
TaskHandle_t app_supervisor_handle;

static void supervisor(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t idle_prev = ulTaskGetIdleRunTimeCounter();
    uint32_t total_prev = portGET_RUN_TIME_COUNTER_VALUE();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));
        wd_quorum_checkin(&app_quorum, WD_BIT_SUPERVISOR);
        if (wd_quorum_evaluate(&app_quorum)) {
            hal_wdt_feed();
        } else {
            hal_wdt_note_missing(app_quorum.last_missing);
        }
        uint32_t idle = ulTaskGetIdleRunTimeCounter();
        uint32_t total = portGET_RUN_TIME_COUNTER_VALUE();
        uint32_t d_total = total - total_prev, d_idle = idle - idle_prev;
        if (d_total) {
            uint32_t busy = d_total > d_idle ? d_total - d_idle : 0;
            /* Scale down first: d_total is ~12e6 per 250 ms, x1000 overflows. */
            uint32_t load = (busy / 1000u) * 1000u / ((d_total / 1000u) ? (d_total / 1000u) : 1u);
            app_cpu.cpu_load_permille = (uint16_t)(load > 1000u ? 1000u : load);
            if (app_cpu.cpu_load_permille > app_cpu.cpu_load_peak_permille)
                app_cpu.cpu_load_peak_permille = app_cpu.cpu_load_permille;
            app_cpu.samples++;
        }
        idle_prev = idle;
        total_prev = total;
    }
}

void supervisor_task_create(void) {
    app_supervisor_handle = xTaskCreateStatic(supervisor, "superv", STACK_SUPERVISOR, 0,
                                              PRIO_SUPERVISOR, stack, &tcb);
    configASSERT(app_supervisor_handle);
}
