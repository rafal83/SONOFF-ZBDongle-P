/* SPDX-License-Identifier: MIT
 * Direct CC26x2 RF core driver for IEEE 802.15.4 (2.4 GHz O-QPSK).
 * Uses the RF core command interface (doorbell) documented in the CC13x2/
 * CC26x2 TRM and TI's BSD headers; does NOT use TI's RF.a, DPL or Power
 * driver, so it runs under FreeRTOS without TI-RTOS.
 *
 * STATUS: compiled and host-tested for its pure logic only. Never executed on
 * an RF core. Every hardware wait is bounded and reports a step code.
 *
 * Threading: one owner task calls every function except rf_isr_*; the ISR
 * only latches CPE flags and calls the notify hook.
 */
#ifndef FRT_RF_IEEE_H
#define FRT_RF_IEEE_H
#include <stdbool.h>
#include <stdint.h>
#include "rf_logic.h"

enum {
    RF_OK = 0,
    RF_ERR_STATE = 1,
    RF_ERR_POWER_DOMAIN = 2,
    RF_ERR_BOOT_TIMEOUT = 3,
    RF_ERR_DOORBELL_BUSY = 4,
    RF_ERR_DOORBELL_ACK = 5,
    RF_ERR_CMDSTA = 6,
    RF_ERR_SETUP = 7,
    RF_ERR_CMD_TIMEOUT = 8,
    RF_ERR_PARAM = 9,
    RF_ERR_RX_NOT_RUNNING = 10,
    RF_ERR_TX_BUSY = 11,
};

typedef enum { RF_TX_DONE = 0, RF_TX_NO_ACK, RF_TX_CHANNEL_BUSY, RF_TX_ERROR } rf_tx_result_t;

typedef struct {
    uint8_t channel;
    bool promiscuous;        /* true: no filtering, no auto-ACK (sniffer) */
    bool pan_coordinator;
    uint16_t pan_id, short_addr;
    uint8_t ext_addr[8];     /* little endian */
} rf_rx_config_t;

typedef struct {
    uint32_t powerups, setup_failures, last_error_step;
    uint32_t rx_frames, rx_crc_fail, rx_ignored, rx_buf_full, rx_overflow_restarts;
    uint32_t rx_dropped_parse;
    uint32_t tx_attempts, tx_ok, tx_no_ack, tx_busy, tx_error, tx_ack_pending;
    uint32_t tx_timed;       /* rf_tx_at transmissions started */
    uint32_t internal_errors, synth_no_lock, src_match_overflow;
    uint32_t isr_count, isr_max_cycles;
    int8_t last_rssi, max_rssi;
    uint8_t channel, power_dbm, high_pa;
    uint16_t last_cmd_status;
    /* First failure of a radio setup, kept until the next success: step
     * (error code << 16 | step) and the three command statuses. */
    uint32_t setup_fail_step, setup_fail_status;   /* status: setup | rat << 16 */
    uint16_t setup_fail_fs_status;
} rf_stats_t;

/* notify is called from the CPE ISR when new events are latched. */
void rf_init(void (*notify_from_isr)(void));
int rf_power_up(int8_t tx_power_dbm);
void rf_power_down(void);
bool rf_is_powered(void);
int rf_rx_start(const rf_rx_config_t *config);
int rf_rx_stop(void);
/* Take the next received frame, if any. Frees its hardware entry. */
bool rf_rx_take(rf_rx_frame_t *out);
/* Timed TX (no CSMA, no ACK) at the absolute radio time `rat_start` (4 MHz
 * RAT, see rf_rat_now), between RF_TX_AT_MIN_LEAD and RF_TX_AT_MAX_LEAD
 * ticks from now; completion through rf_tx_poll (RF_TX_DONE / RF_TX_ERROR). */
#define RF_TX_AT_MIN_LEAD 1200u      /* 300 us: command setup before the trigger */
#define RF_TX_AT_MAX_LEAD 200000u    /* 50 ms */
int rf_tx_at(const uint8_t *mpdu, uint8_t length, uint32_t rat_start);
/* Start CSMA-CA + TX (+ ACK wait). mpdu excludes the FCS. RX must run. */
int rf_tx_start(const uint8_t *mpdu, uint8_t length, bool wait_ack, uint8_t max_backoffs);
/* Poll after a notify: returns true once the TX chain finished. */
bool rf_tx_poll(rf_tx_result_t *result, bool *frame_pending);
/* Abort a TX chain whose completion was not observed in time. */
void rf_tx_abort(void);
/* Set/clear the ACK frame-pending bit for a source (MAC indirect queue). */
int rf_source_match(bool extended, uint64_t address, bool pending);
uint32_t rf_take_events(void);
/* Radio timer (4 MHz), same time base as rf_rx_frame_t.timestamp; 0 when off. */
uint32_t rf_rat_now(void);
void rf_get_stats(rf_stats_t *out);
#endif
