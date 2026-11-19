/* SPDX-License-Identifier: MIT
 * Original CC26x2 IEEE 802.15.4 RF core driver. The power-up order follows
 * the CC13x2/CC26x2 TRM (SWCU185) chapter "Radio" and the observable order
 * of TI's BSD RFCC26X2_multiMode.c: RFCBITS boot word, mode select, RFCORE
 * domain, clocks, BOOT_DONE, CPE patch, bus request, SETUP -> SYNC_START_RAT
 * -> FS. NOT validated on hardware.
 */
#include "rf_ieee.h"
#include "rf_settings.h"
#include "board.h"
#include "hal_sys.h"

#include <inc/hw_aon_rtc.h>
#include <inc/hw_ints.h>
#include <inc/hw_memmap.h>
#include <inc/hw_prcm.h>
#include <inc/hw_rfc_dbell.h>
#include <inc/hw_types.h>
#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>
#include <driverlib/rfc.h>
#include <driverlib/rf_common_cmd.h>
#include <driverlib/rf_data_entry.h>
#include <driverlib/rf_ieee_cmd.h>
#include <driverlib/rf_ieee_mailbox.h>
#include <driverlib/rf_mailbox.h>
#include <rf_patches/rf_patch_cpe_ieee_802_15_4.h>

#define RF_BOOT0 0xE0000011u
#define RF_BOOT1 0x00000080u
#define WAIT_BUDGET_CYCLES (48000000u / 20u)   /* 50 ms */
#define RX_ENTRIES 4u
#define RX_ENTRY_DATA 136u                     /* >= RF_RX_ELEMENT_MAX */
#define ACK_TIMEOUT_RAT 4000u                  /* 1 ms at 4 MHz (54 symbols = 864 us) */

#define CPE0_EVENTS (RFC_DBELL_RFCPEIFG_RX_ENTRY_DONE | RFC_DBELL_RFCPEIFG_RX_BUF_FULL | \
                     RFC_DBELL_RFCPEIFG_LAST_FG_COMMAND_DONE | RFC_DBELL_RFCPEIFG_LAST_COMMAND_DONE)
#define CPE1_EVENTS (RFC_DBELL_RFCPEIFG_INTERNAL_ERROR | RFC_DBELL_RFCPEIFG_SYNTH_NO_LOCK)

typedef struct {
    rfc_dataEntryGeneral_t header;
    uint8_t pad[RX_ENTRY_DATA - 1];      /* header.data is the first byte */
} __attribute__((aligned(4))) rx_entry_t;

static rx_entry_t rx_entries[RX_ENTRIES];
static unsigned rx_read_index;
static dataQueue_t rx_queue;
static rfc_ieeeRxOutput_t rx_output;

static rfc_CMD_RADIO_SETUP_PA_t cmd_setup;
static rfc_CMD_SYNC_START_RAT_t cmd_rat;
static rfc_CMD_FS_t cmd_fs;
static rfc_CMD_IEEE_RX_t cmd_rx;
static rfc_CMD_IEEE_CSMA_t cmd_csma;
static rfc_CMD_IEEE_TX_t cmd_tx;
static rfc_CMD_IEEE_RX_ACK_t cmd_rx_ack;
static uint8_t tx_buffer[RF_MAX_MPDU_NO_FCS];

static volatile uint32_t pending_events;
static void (*notify)(void);
static rf_stats_t stats;
static bool powered, rx_running, tx_active, tx_wait_ack;
static rf_rx_config_t rx_config;
static uint16_t csma_seed = 0x1234;
static rf_src_match_t src_match;

#define FINISHED(status) (((status) & 0x0C00u) != 0u)

static uint32_t elapsed_since(uint32_t start) { return hal_cycles() - start; }

static int fail(int code, uint32_t step) {
    stats.last_error_step = step;
    return code;
}

static int doorbell(uint32_t command, uint32_t *cmdsta) {
    uint32_t start = hal_cycles();
    while (HWREG(RFC_DBELL_BASE + RFC_DBELL_O_CMDR) != 0) {
        if (elapsed_since(start) > WAIT_BUDGET_CYCLES) return fail(RF_ERR_DOORBELL_BUSY, command);
    }
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFACKIFG) = 0;
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_CMDR) = command;
    while (!HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFACKIFG)) {
        if (elapsed_since(start) > WAIT_BUDGET_CYCLES) return fail(RF_ERR_DOORBELL_ACK, command);
    }
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFACKIFG) = 0;
    uint32_t status = HWREG(RFC_DBELL_BASE + RFC_DBELL_O_CMDSTA);
    if (cmdsta) *cmdsta = status;
    return (status & 0xFFu) == CMDSTA_Done ? RF_OK : fail(RF_ERR_CMDSTA, status);
}

static int wait_finished(volatile uint16_t *status, uint32_t step) {
    uint32_t start = hal_cycles();
    while (!FINISHED(*status)) {
        if (elapsed_since(start) > WAIT_BUDGET_CYCLES) return fail(RF_ERR_CMD_TIMEOUT, step);
    }
    stats.last_cmd_status = *status;
    return RF_OK;
}

static void antenna_off(void) {
    static const uint8_t pins[] = {BOARD_RF_24GHZ_DIO, BOARD_RF_HIGH_PA_DIO, BOARD_RF_SUB1GHZ_DIO};
    for (unsigned i = 0; i < sizeof pins; i++) {
        IOCPortConfigureSet(pins[i], IOC_PORT_GPIO, IOC_STD_OUTPUT);
        GPIO_clearDio(pins[i]);
        GPIO_setOutputEnableDio(pins[i], GPIO_OUTPUT_ENABLE);
    }
}

static void antenna_24ghz(bool high_pa) {
    antenna_off();
    if (high_pa) {
        /* LNA enable and PA enable driven by the RF core (TI workaround uses
         * GPO3 for the PA enable on CC1352P-2 class boards). */
        IOCPortConfigureSet(BOARD_RF_24GHZ_DIO, IOC_PORT_RFC_GPO0, IOC_STD_OUTPUT);
        IOCPortConfigureSet(BOARD_RF_HIGH_PA_DIO, IOC_PORT_RFC_GPO3, IOC_STD_OUTPUT);
    } else {
        GPIO_setDio(BOARD_RF_24GHZ_DIO);
    }
}

static void configure_pa(const rf_pa_entry_t *pa) {
    if (pa->high_pa) {
        cmd_setup.txPower = RF_TX20_ENABLED;
        rf_override_set_power(rf_overrides_tx20, 7, RF_TX20_PATTERN, pa->raw);
        rf_override_attach(rf_overrides_base, 4, (uint32_t)(uintptr_t)rf_overrides_tx20);
    } else {
        cmd_setup.txPower = (uint16_t)pa->raw;
        rf_override_set_power(rf_overrides_tx_std, 7, RF_TX_STD_PATTERN, pa->raw);
        rf_override_detach(rf_overrides_base, 4, (uint32_t)(uintptr_t)rf_overrides_tx20);
    }
    stats.power_dbm = (uint8_t)pa->dbm;
    stats.high_pa = pa->high_pa;
}

void rf_init(void (*notify_from_isr)(void)) {
    notify = notify_from_isr;
    antenna_off();
    IntPrioritySet(INT_RFC_CPE_0, BOARD_IRQ_PRIO_RF);
    IntPrioritySet(INT_RFC_CPE_1, BOARD_IRQ_PRIO_RF);
}

bool rf_is_powered(void) { return powered; }

uint32_t rf_rat_now(void) { return powered ? HWREG(RFC_RAT_BASE + 0x04u /* RATCNT */) : 0u; }

int rf_power_up(int8_t tx_power_dbm) {
    if (powered) return fail(RF_ERR_STATE, 0x100);
    const rf_pa_entry_t *pa = rf_pa_lookup(rf_pa_table, rf_pa_table_count, tx_power_dbm);
    stats.powerups++;
    rf_src_match_init(&src_match);

    HWREG(AON_RTC_BASE + AON_RTC_O_CTL) |= AON_RTC_CTL_RTC_UPD_EN_M;
    HWREG(PRCM_BASE + PRCM_O_RFCBITS) = RF_BOOT0;
    HWREG(PRCM_BASE + PRCM_O_RFCMODESEL) = RF_MODE_AUTO;
    PRCMPowerDomainOn(PRCM_DOMAIN_RFCORE);
    uint32_t start = hal_cycles();
    while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_ON) {
        if (elapsed_since(start) > WAIT_BUDGET_CYCLES) return fail(RF_ERR_POWER_DOMAIN, 0x101);
    }
    RFCClockEnable();
    RFCCpeIntDisable(0xFFFFFFFFu);
    RFCCpeIntClear(0xFFFFFFFFu);
    RFCCpe0IntSelect(CPE0_EVENTS | RFC_DBELL_RFCPEIFG_BOOT_DONE);
    RFCCpe1IntSelect(CPE1_EVENTS);
    start = hal_cycles();
    while (!(HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIFG) & RFC_DBELL_RFCPEIFG_BOOT_DONE)) {
        if (elapsed_since(start) > WAIT_BUDGET_CYCLES) {
            rf_power_down();
            return fail(RF_ERR_BOOT_TIMEOUT, 0x102);
        }
    }
    RFCCpeIntClear(RFC_DBELL_RFCPEIFG_BOOT_DONE);
    rf_patch_cpe_ieee_802_15_4();
    powered = true;
    int rc = doorbell(CMDR_DIR_CMD_1BYTE(CMD_BUS_REQUEST, 1), 0);
    if (rc) { rf_power_down(); return rc; }

    cmd_setup = (rfc_CMD_RADIO_SETUP_PA_t){0};
    cmd_setup.commandNo = CMD_RADIO_SETUP_PA;
    cmd_setup.startTrigger.triggerType = TRIG_NOW;
    cmd_setup.condition.rule = COND_ALWAYS;
    cmd_setup.pNextOp = (rfc_radioOp_t *)&cmd_rat;
    cmd_setup.mode = 0x01;           /* IEEE 802.15.4 */
    cmd_setup.config.biasMode = 1;
    cmd_setup.pRegOverride = rf_overrides_base;
    cmd_setup.pRegOverrideTxStd = rf_overrides_tx_std;
    cmd_setup.pRegOverrideTx20 = rf_overrides_tx20;
    configure_pa(pa);

    cmd_rat = (rfc_CMD_SYNC_START_RAT_t){0};
    cmd_rat.commandNo = CMD_SYNC_START_RAT;
    cmd_rat.startTrigger.triggerType = TRIG_NOW;
    cmd_rat.condition.rule = COND_ALWAYS;
    cmd_rat.pNextOp = (rfc_radioOp_t *)&cmd_fs;
    cmd_rat.rat0 = 0;

    cmd_fs = (rfc_CMD_FS_t){0};
    cmd_fs.commandNo = CMD_FS;
    cmd_fs.startTrigger.triggerType = TRIG_NOW;
    cmd_fs.condition.rule = COND_NEVER;
    cmd_fs.frequency = rf_channel_to_mhz(RF_CHANNEL_MIN);

    rc = doorbell((uint32_t)(uintptr_t)&cmd_setup, 0);
    if (!rc) rc = wait_finished(&cmd_fs.status, 0x103);
    if (rc || cmd_setup.status != DONE_OK || cmd_rat.status != DONE_OK || cmd_fs.status != DONE_OK) {
        stats.setup_fail_step = ((uint32_t)(rc & 0xFFFF) << 16) | (stats.last_error_step & 0xFFFFu);
        stats.setup_fail_status = cmd_setup.status | ((uint32_t)cmd_rat.status << 16);
        stats.setup_fail_fs_status = cmd_fs.status;
    }
    if (!rc && (cmd_setup.status != DONE_OK || cmd_rat.status != DONE_OK || cmd_fs.status != DONE_OK)) {
        stats.last_cmd_status = cmd_setup.status != DONE_OK ? cmd_setup.status :
                                cmd_rat.status != DONE_OK ? cmd_rat.status : cmd_fs.status;
        rc = fail(RF_ERR_SETUP, 0x104);
    }
    if (rc) {
        stats.setup_failures++;
        rf_power_down();
        return rc;
    }
    HWREG(PRCM_BASE + PRCM_O_RFCBITS) |= RF_BOOT1;
    antenna_24ghz(pa->high_pa);
    RFCCpeIntClear(0xFFFFFFFFu);
    RFCCpeIntEnable(CPE0_EVENTS | CPE1_EVENTS);
    IntPendClear(INT_RFC_CPE_0);
    IntPendClear(INT_RFC_CPE_1);
    IntEnable(INT_RFC_CPE_0);
    IntEnable(INT_RFC_CPE_1);
    return RF_OK;
}

void rf_power_down(void) {
    IntDisable(INT_RFC_CPE_0);
    IntDisable(INT_RFC_CPE_1);
    if (powered) {
        (void)doorbell(CMDR_DIR_CMD(CMD_ABORT), 0);
    }
    RFCCpeIntDisable(0xFFFFFFFFu);
    RFCCpeIntClear(0xFFFFFFFFu);
    antenna_off();
    RFCClockDisable();
    PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE);
    /* Wait for the domain to be really off: powering it on again too early
     * finds the RF core still up, without a fresh boot. */
    uint32_t start = hal_cycles();
    while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_OFF &&
           elapsed_since(start) < WAIT_BUDGET_CYCLES) {
    }
    powered = rx_running = tx_active = false;
}

static void rx_queue_init(void) {
    for (unsigned i = 0; i < RX_ENTRIES; i++) {
        rfc_dataEntryGeneral_t *e = &rx_entries[i].header;
        e->pNextEntry = (uint8_t *)&rx_entries[(i + 1u) % RX_ENTRIES];
        e->status = DATA_ENTRY_PENDING;
        e->config.type = DATA_ENTRY_TYPE_GEN;
        e->config.lenSz = 1;
        e->config.irqIntv = 0;
        e->length = RX_ENTRY_DATA;
    }
    rx_queue.pCurrEntry = (uint8_t *)&rx_entries[0];
    rx_queue.pLastEntry = 0;    /* circular */
    rx_read_index = 0;
}

int rf_rx_start(const rf_rx_config_t *config) {
    if (!powered) return fail(RF_ERR_STATE, 0x200);
    if (rf_channel_to_mhz(config->channel) == 0) return fail(RF_ERR_PARAM, 0x201);
    if (rx_running) (void)rf_rx_stop();
    rx_config = *config;
    rx_queue_init();
    rx_output = (rfc_ieeeRxOutput_t){0};
    cmd_rx = (rfc_CMD_IEEE_RX_t){0};
    cmd_rx.commandNo = CMD_IEEE_RX;
    cmd_rx.startTrigger.triggerType = TRIG_NOW;
    cmd_rx.condition.rule = COND_NEVER;
    cmd_rx.channel = config->channel;
    cmd_rx.rxConfig.bAutoFlushCrc = 1;
    cmd_rx.rxConfig.bAutoFlushIgn = config->promiscuous ? 0 : 1;
    cmd_rx.rxConfig.bAppendRssi = 1;
    cmd_rx.rxConfig.bAppendCorrCrc = 1;
    cmd_rx.rxConfig.bAppendTimestamp = 1;
    cmd_rx.pRxQ = &rx_queue;
    cmd_rx.pOutput = &rx_output;
    cmd_rx.frameFiltOpt.frameFiltEn = config->promiscuous ? 0 : 1;
    cmd_rx.frameFiltOpt.autoAckEn = config->promiscuous ? 0 : 1;
    cmd_rx.frameFiltOpt.bPanCoord = config->pan_coordinator ? 1 : 0;
    cmd_rx.frameFiltOpt.maxFrameVersion = 2;
    if (!config->promiscuous) {
        /* Hardware frame-pending bit in ACKs of data requests only, from the
         * source-matching lists maintained by rf_source_match(). */
        cmd_rx.frameFiltOpt.autoPendEn = 1;
        cmd_rx.frameFiltOpt.bPendDataReqOnly = 1;
        cmd_rx.numShortEntries = RF_SRC_SHORT_ENTRIES;
        cmd_rx.numExtEntries = RF_SRC_EXT_ENTRIES;
        cmd_rx.pShortEntryList = (uint32_t *)&src_match.short_list;
        cmd_rx.pExtEntryList = (uint32_t *)&src_match.ext_list;
    }
    cmd_rx.frameTypes.bAcceptFt0Beacon = 1;
    cmd_rx.frameTypes.bAcceptFt1Data = 1;
    cmd_rx.frameTypes.bAcceptFt2Ack = config->promiscuous ? 1 : 0;
    cmd_rx.frameTypes.bAcceptFt3MacCmd = 1;
    cmd_rx.ccaOpt.ccaEnEnergy = 1;
    cmd_rx.ccaOpt.ccaEnCorr = 1;
    cmd_rx.ccaOpt.ccaEnSync = 1;
    cmd_rx.ccaOpt.ccaCorrOp = 1;
    cmd_rx.ccaOpt.ccaSyncOp = 0;
    cmd_rx.ccaOpt.ccaCorrThr = 3;
    cmd_rx.ccaRssiThr = -75;  /* energy threshold (dBm), to be tuned on hardware */
    cmd_rx.localPanID = config->pan_id;
    cmd_rx.localShortAddr = config->short_addr;
    uint64_t ext = 0;
    for (int i = 7; i >= 0; i--) ext = (ext << 8) | config->ext_addr[i];
    cmd_rx.localExtAddr = ext;
    cmd_rx.endTrigger.triggerType = TRIG_NEVER;
    int rc = doorbell((uint32_t)(uintptr_t)&cmd_rx, 0);
    if (rc) return rc;
    rx_running = true;
    stats.channel = config->channel;
    return RF_OK;
}

int rf_rx_stop(void) {
    if (!powered) return fail(RF_ERR_STATE, 0x300);
    int rc = doorbell(CMDR_DIR_CMD(CMD_ABORT), 0);
    rx_running = tx_active = false;
    return rc;
}

bool rf_rx_take(rf_rx_frame_t *out) {
    for (unsigned guard = 0; guard < RX_ENTRIES; guard++) {
        rfc_dataEntryGeneral_t *e = &rx_entries[rx_read_index].header;
        if (e->status != DATA_ENTRY_FINISHED) return false;
        bool ok = rf_rx_parse_element(&e->data, RX_ENTRY_DATA, out);
        e->status = DATA_ENTRY_PENDING;
        rx_read_index = (rx_read_index + 1u) % RX_ENTRIES;
        if (ok) {
            stats.rx_frames++;
            stats.last_rssi = out->rssi;
            if (out->rssi > stats.max_rssi) stats.max_rssi = out->rssi;
            return true;
        }
        stats.rx_dropped_parse++;
    }
    return false;
}

int rf_tx_start(const uint8_t *mpdu, uint8_t length, bool wait_ack, uint8_t max_backoffs) {
    if (!powered || !rx_running) return fail(RF_ERR_RX_NOT_RUNNING, 0x400);
    if (tx_active) return fail(RF_ERR_TX_BUSY, 0x401);
    if (length < 3 || length > RF_MAX_MPDU_NO_FCS) return fail(RF_ERR_PARAM, 0x402);
    for (uint8_t i = 0; i < length; i++) tx_buffer[i] = mpdu[i];
    csma_seed = (uint16_t)(csma_seed * 31421u + 6927u) ^ (uint16_t)hal_cycles();

    cmd_csma = (rfc_CMD_IEEE_CSMA_t){0};
    cmd_csma.commandNo = CMD_IEEE_CSMA;
    cmd_csma.startTrigger.triggerType = TRIG_NOW;
    cmd_csma.condition.rule = COND_STOP_ON_FALSE;
    cmd_csma.pNextOp = (rfc_radioOp_t *)&cmd_tx;
    cmd_csma.randomState = csma_seed;
    cmd_csma.macMaxBE = 5;
    cmd_csma.macMaxCSMABackoffs = max_backoffs;
    cmd_csma.csmaConfig.initCW = 1;
    cmd_csma.BE = 3;
    cmd_csma.endTrigger.triggerType = TRIG_NEVER;

    cmd_tx = (rfc_CMD_IEEE_TX_t){0};
    cmd_tx.commandNo = CMD_IEEE_TX;
    cmd_tx.startTrigger.triggerType = TRIG_NOW;
    cmd_tx.condition.rule = wait_ack ? COND_STOP_ON_FALSE : COND_NEVER;
    cmd_tx.pNextOp = wait_ack ? (rfc_radioOp_t *)&cmd_rx_ack : 0;
    cmd_tx.payloadLen = length;
    cmd_tx.pPayload = tx_buffer;

    cmd_rx_ack = (rfc_CMD_IEEE_RX_ACK_t){0};
    cmd_rx_ack.commandNo = CMD_IEEE_RX_ACK;
    cmd_rx_ack.startTrigger.triggerType = TRIG_NOW;
    cmd_rx_ack.condition.rule = COND_NEVER;
    cmd_rx_ack.seqNo = tx_buffer[2];
    cmd_rx_ack.endTrigger.triggerType = TRIG_REL_START;
    cmd_rx_ack.endTime = ACK_TIMEOUT_RAT;

    tx_wait_ack = wait_ack;
    stats.tx_attempts++;
    int rc = doorbell((uint32_t)(uintptr_t)&cmd_csma, 0);
    if (rc) { stats.tx_error++; return rc; }
    tx_active = true;
    return RF_OK;
}

bool rf_tx_poll(rf_tx_result_t *result, bool *frame_pending) {
    if (!tx_active) return false;
    volatile uint16_t *last = tx_wait_ack ? &cmd_rx_ack.status : &cmd_tx.status;
    /* The chain stops early on CSMA failure or TX failure. */
    bool csma_failed = FINISHED(cmd_csma.status) && cmd_csma.status != IEEE_DONE_OK;
    bool tx_failed = FINISHED(cmd_tx.status) && cmd_tx.status != IEEE_DONE_OK;
    if (!csma_failed && !tx_failed && !FINISHED(*last)) return false;
    tx_active = false;
    *frame_pending = false;
    if (csma_failed) {
        *result = cmd_csma.status == IEEE_DONE_BUSY ? RF_TX_CHANNEL_BUSY : RF_TX_ERROR;
    } else if (tx_failed) {
        *result = RF_TX_ERROR;
    } else if (!tx_wait_ack) {
        *result = RF_TX_DONE;
    } else if (cmd_rx_ack.status == IEEE_DONE_ACK || cmd_rx_ack.status == IEEE_DONE_ACKPEND) {
        *result = RF_TX_DONE;
        *frame_pending = cmd_rx_ack.status == IEEE_DONE_ACKPEND;
    } else if (cmd_rx_ack.status == IEEE_DONE_TIMEOUT) {
        *result = RF_TX_NO_ACK;
    } else {
        *result = RF_TX_ERROR;
    }
    switch (*result) {
    case RF_TX_DONE: stats.tx_ok++; if (*frame_pending) stats.tx_ack_pending++; break;
    case RF_TX_NO_ACK: stats.tx_no_ack++; break;
    case RF_TX_CHANNEL_BUSY: stats.tx_busy++; break;
    default: stats.tx_error++; stats.last_cmd_status = cmd_tx.status; break;
    }
    return true;
}

void rf_tx_abort(void) {
    if (tx_active) {
        (void)doorbell(CMDR_DIR_CMD(CMD_IEEE_ABORT_FG), 0);
        tx_active = false;
        stats.tx_error++;
    }
}

uint32_t rf_take_events(void) {
    IntDisable(INT_RFC_CPE_0);
    IntDisable(INT_RFC_CPE_1);
    uint32_t events = pending_events;
    pending_events = 0;
    if (powered) {
        IntEnable(INT_RFC_CPE_0);
        IntEnable(INT_RFC_CPE_1);
    }
    if ((events & RFC_DBELL_RFCPEIFG_LAST_COMMAND_DONE) && rx_running && FINISHED(cmd_rx.status)) {
        /* Background RX ended (e.g. overflow or error): restart it. */
        stats.rx_overflow_restarts++;
        stats.last_cmd_status = cmd_rx.status;
        rx_running = false;
        rf_rx_config_t again = rx_config;
        (void)rf_rx_start(&again);
    }
    stats.rx_crc_fail = rx_output.nRxNok;      /* 8-bit hardware counters */
    stats.rx_ignored = rx_output.nRxIgnored;
    stats.rx_buf_full = rx_output.nRxBufFull;
    return events;
}

void rf_get_stats(rf_stats_t *out) {
    IntDisable(INT_RFC_CPE_0);
    IntDisable(INT_RFC_CPE_1);
    *out = stats;
    if (powered) {
        IntEnable(INT_RFC_CPE_0);
        IntEnable(INT_RFC_CPE_1);
    }
}

static void cpe_isr(void) {
    uint32_t start = hal_cycles();
    uint32_t flags = HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIFG) &
                     HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIEN);
    HWREG(RFC_DBELL_BASE + RFC_DBELL_O_RFCPEIFG) = ~flags;
    if (flags & RFC_DBELL_RFCPEIFG_INTERNAL_ERROR) stats.internal_errors++;
    if (flags & RFC_DBELL_RFCPEIFG_SYNTH_NO_LOCK) stats.synth_no_lock++;
    pending_events |= flags;
    stats.isr_count++;
    uint32_t spent = hal_cycles() - start;
    if (spent > stats.isr_max_cycles) stats.isr_max_cycles = spent;
    if (flags && notify) notify();
}

int rf_source_match(bool extended, uint64_t address, bool pending) {
    bool running = powered && rx_running;
    bool full = false;
    uint8_t entry = rf_src_match_update(&src_match, extended, address, rx_config.pan_id, pending, !running, &full);
    if (full) { stats.src_match_overflow++; return fail(RF_ERR_PARAM, 0x500); }
    if (entry == RF_SRC_NONE || !running) return RF_OK;
    rfc_CMD_IEEE_MOD_SRC_MATCH_t cmd = {0};
    cmd.commandNo = CMD_IEEE_MOD_SRC_MATCH;
    cmd.options.bEnable = pending;
    cmd.options.srcPend = pending;
    cmd.options.entryType = extended;
    cmd.entryNo = entry;
    return doorbell((uint32_t)(uintptr_t)&cmd, 0);
}

void RFC_CPE0_IRQHandler(void) { cpe_isr(); }
void RFC_CPE1_IRQHandler(void) { cpe_isr(); }
