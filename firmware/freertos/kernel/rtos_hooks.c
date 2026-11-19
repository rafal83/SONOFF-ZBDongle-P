/* SPDX-License-Identifier: MIT
 * Static-allocation callbacks and failure hooks required by FreeRTOS.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "crash_log.h"
#include "hal_sys.h"

static StaticTask_t idle_tcb;
static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
#if configUSE_TIMERS
static StaticTask_t timer_tcb;
static StackType_t timer_stack[configTIMER_TASK_STACK_DEPTH];
#endif

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *size) {
    *tcb = &idle_tcb;
    *stack = idle_stack;
    *size = configMINIMAL_STACK_SIZE;
}

#if configUSE_TIMERS
void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                    configSTACK_DEPTH_TYPE *size) {
    *tcb = &timer_tcb;
    *stack = timer_stack;
    *size = configTIMER_TASK_STACK_DEPTH;
}
#endif

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    crash_log_record_text(CRASH_STACK_OVERFLOW, name, 0);
}

void rtos_assert_failed(const char *file, unsigned line) {
    crash_log_record_text(CRASH_ASSERT, file, line);
}

void vApplicationIdleHook(void) {
    /* Sleep until the next interrupt (CPU clock gated, no deep sleep: SCR
     * SLEEPDEEP stays 0, peripherals and RF core keep running). */
    __asm volatile("dsb\n wfi\n isb" ::: "memory");
}

void rtos_runtime_counter_init(void) { hal_cycle_counter_init(); }
uint32_t rtos_runtime_counter(void) { return hal_cycles(); }
