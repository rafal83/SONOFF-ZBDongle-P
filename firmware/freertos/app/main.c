/* SPDX-License-Identifier: MIT
 * FreeRTOS target entry point: hardware bring-up, static task creation,
 * scheduler start. No Zigbee stack yet (see docs/freertos-migration/).
 */
#include "app.h"
#include "hal_sys.h"
#include "hal_wdt.h"
#include "hal_uart.h"
#include "board.h"
#include "crash_log.h"
#include "le_buf.h"

wd_quorum_t app_quorum;
uint32_t app_init_status;
uint8_t app_ieee[8];

/* AREQ 0x5F 0x81: boot milestone [stage, init_status u32, reset source,
 * previous crash kind]. Polled, before the scheduler. Only on an ABNORMAL
 * boot (init failure or previous crash): a normal boot sends nothing but
 * the ZNP SYS_RESET_IND, since a ZNP host logs unknown frames as errors
 * (seen with Zigbee2MQTT, 18 November). */
static void milestone(uint8_t stage) {
    if (app_init_status == 0 && crash_log_previous()->kind == CRASH_NONE) return;
    uint8_t p[7];
    le_writer_t w = le_writer(p, sizeof p);
    le_u8(&w, stage);
    le_u32(&w, app_init_status);
    le_u8(&w, (uint8_t)hal_sys_reset_source());
    le_u8(&w, (uint8_t)crash_log_previous()->kind);
    hal_uart_polled_frame(0x81, p, (uint8_t)w.len);
}

/* AREQ 0x5F 0x82: crash record sent just before the reset. */
void crash_log_emit(const crash_record_t *c) {
    uint8_t p[52];
    le_writer_t w = le_writer(p, sizeof p);
    le_u8(&w, (uint8_t)c->kind);
    le_u32(&w, c->pc);
    le_u32(&w, c->lr);
    le_u32(&w, c->psr);
    le_u32(&w, c->cfsr);
    le_u32(&w, c->hfsr);
    le_u32(&w, c->bfar);
    le_u32(&w, c->detail);
    le_bytes(&w, c->where, 16);
    hal_uart_polled_frame(0x82, p, (uint8_t)w.len);
}

int main(void) {
    hal_cycle_counter_init();
    /* A clock/power failure is reported over MT later, not hidden: the UART
     * itself still runs from the reset-default HF RC oscillator if the XOSC
     * switch failed, at the configured baud rate (48 MHz nominal). */
    app_init_status = hal_sys_init();
    hal_sys_ieee_address(app_ieee);
    hal_uart_early_init(BOARD_UART_BAUD);
    milestone(1);
    wd_quorum_init(&app_quorum);
    (void)wd_quorum_register(&app_quorum, WD_BIT_HOST);
    (void)wd_quorum_register(&app_quorum, WD_BIT_RADIO);
    (void)wd_quorum_register(&app_quorum, WD_BIT_SUPERVISOR);

    host_task_create();
    radio_task_create();
    supervisor_task_create();
    milestone(2);
    hal_wdt_start(WATCHDOG_TIMEOUT_MS);
    milestone(3);
    vTaskStartScheduler();
    /* Only reached if the idle/timer tasks could not be created. */
    configASSERT(0);
    return 0;
}
