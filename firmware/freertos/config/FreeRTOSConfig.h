/* SPDX-License-Identifier: MIT
 * FreeRTOS kernel configuration for the SONOFF ZBDongle-P (CC2652P, Cortex-M4F).
 *
 * Design rules (docs/freertos-migration/architecture.md):
 *  - static allocation only: no FreeRTOS heap is linked (heap_x.c absent);
 *  - preemptive scheduling, 1 kHz tick, no tickless idle (RF core stays on);
 *  - every ISR that calls a FromISR API runs at a priority numerically
 *    >= configMAX_SYSCALL_INTERRUPT_PRIORITY.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

#define configCPU_CLOCK_HZ                      48000000u
#define configTICK_RATE_HZ                      1000u
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0
#define configMAX_PRIORITIES                    6
#define configMINIMAL_STACK_SIZE                128u
#define configMAX_TASK_NAME_LEN                 8
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   2
#define configQUEUE_REGISTRY_SIZE               0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_MINI_LIST_ITEM                1
#define configSTACK_DEPTH_TYPE                  uint16_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t
#define configHEAP_CLEAR_MEMORY_ON_FREE         0
#define configUSE_NEWLIB_REENTRANT              0

/* Memory: static only. A dynamic call fails to link instead of silently
 * using an unbounded allocator. */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        0
#define configKERNEL_PROVIDED_STATIC_MEMORY     0
#define configAPPLICATION_ALLOCATED_HEAP        0

/* Hooks */
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
#define configCHECK_FOR_STACK_OVERFLOW          2

/* Run-time statistics: DWT cycle counter (48 MHz). Per-window deltas are
 * computed with unsigned arithmetic; windows must stay below 89 s. */
#define configGENERATE_RUN_TIME_STATS           1
#define configRUN_TIME_COUNTER_TYPE             uint32_t
void rtos_runtime_counter_init(void);
uint32_t rtos_runtime_counter(void);
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() rtos_runtime_counter_init()
#define portGET_RUN_TIME_COUNTER_VALUE()         rtos_runtime_counter()
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configRECORD_STACK_HIGH_ADDRESS         1

/* Synchronisation primitives */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_CO_ROUTINES                   0
#define configUSE_EVENT_GROUPS                  0
#define configUSE_STREAM_BUFFERS                0

/* Software timers: one static daemon task. */
#define configUSE_TIMERS                        0   /* no software timer: no timer task (1.2 KB) */
#define configTIMER_TASK_PRIORITY               4
#define configTIMER_QUEUE_LENGTH                8
#define configTIMER_TASK_STACK_DEPTH            256u

/* Cortex-M interrupt priorities. CC26x2 implements 3 priority bits. */
#define configPRIO_BITS                         3
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 7
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 1
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_API_CALL_INTERRUPT_PRIORITY   configMAX_SYSCALL_INTERRUPT_PRIORITY

/* Optional API */
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_xTaskGetHandle                  0
#define INCLUDE_xTaskResumeFromISR              0

/* configASSERT records the location in no-init RAM, then resets. */
void rtos_assert_failed(const char *file, unsigned line);
#define configASSERT(x) do { if (!(x)) rtos_assert_failed(__FILE__, __LINE__); } while (0)

/* Map the port handlers to the names used by the vector table. */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif
