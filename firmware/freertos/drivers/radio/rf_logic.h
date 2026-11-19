/* SPDX-License-Identifier: MIT
 * Hardware-independent parts of the CC26x2 IEEE 802.15.4 radio driver.
 * Host-testable: no register access, no RTOS.
 */
#ifndef FRT_RF_LOGIC_H
#define FRT_RF_LOGIC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_CHANNEL_MIN 11u
#define RF_CHANNEL_MAX 26u
#define RF_MAX_PSDU 127u           /* includes the 2-byte FCS */
#define RF_MAX_MPDU_NO_FCS 125u

/* RX element appended fields (CMD_IEEE_RX with bAppendRssi, bAppendCorrCrc,
 * bAppendTimestamp, lenSz = 1, bIncludePhyHdr = 0, bIncludeCrc = 0). */
#define RF_RX_APPENDED 6u
#define RF_RX_ELEMENT_MAX (1u + RF_MAX_MPDU_NO_FCS + RF_RX_APPENDED)

typedef struct {
    uint8_t mpdu[RF_MAX_MPDU_NO_FCS];
    uint8_t length;        /* MPDU bytes, FCS excluded (checked by hardware) */
    int8_t rssi;           /* dBm */
    uint8_t correlation;   /* 6-bit correlation value */
    uint8_t lqi;           /* RSSI-derived link quality 0..255 (see .c) */
    bool crc_ok;
    bool ignored;          /* frame filtering marked it as ignore */
    uint32_t timestamp;    /* RAT ticks (4 MHz) at start of frame */
} rf_rx_frame_t;

/* MHz value programmed in CMD_FS for a channel; 0 for an invalid channel. */
uint16_t rf_channel_to_mhz(uint8_t channel);

/* Parse one finished RX element (starting with its length byte). Returns
 * false for malformed lengths: never reads past `capacity`. */
bool rf_rx_parse_element(const uint8_t *element, size_t capacity, rf_rx_frame_t *out);
uint8_t rf_rssi_to_lqi(int8_t rssi);

/* Override list segment linking (CMD_RADIO_SETUP_PA). Addresses are given
 * as 32-bit values so the logic is identical on the host and the MCU. */
#define RF_END_OVERRIDE 0xFFFFFFFFu
uint32_t rf_override_segment(uint32_t address);
/* Replace the END marker of `base` (or an existing link to `segment`) by a
 * link to `segment`. `base_len` bounds the scan. Returns false if neither is
 * found within the bound. */
bool rf_override_attach(uint32_t *base, size_t base_len, uint32_t segment_address);
/* Terminate `base` at the link to `segment`, if present. */
bool rf_override_detach(uint32_t *base, size_t base_len, uint32_t segment_address);
/* Replace the gain field of the first override matching `pattern` under the
 * TX power mask. Returns false if absent. */
bool rf_override_set_power(uint32_t *list, size_t len, uint32_t pattern, uint32_t raw);

/* PA selection for a requested power: nearest entry not above `dbm`. */
typedef struct { int8_t dbm; bool high_pa; uint32_t raw; } rf_pa_entry_t;
const rf_pa_entry_t *rf_pa_lookup(const rf_pa_entry_t *table, size_t count, int8_t dbm);

/* Source-matching lists read by CMD_IEEE_RX (auto-pend in ACKs of data
 * requests). Layout per TI rf_ieee_cmd.h: enable bit words, pending bit
 * words, then the entries. `used` is the driver's own allocation shadow:
 * while RX runs, the RF core owns the enable/pending words and they are only
 * changed through CMD_IEEE_MOD_SRC_MATCH. */
#define RF_SRC_SHORT_ENTRIES 24u   /* = core_indirect global quota */
#define RF_SRC_EXT_ENTRIES 4u      /* = MAC extended-address transactions */
typedef struct {
    uint32_t enable[1], pending[1];
    uint32_t entry[RF_SRC_SHORT_ENTRIES];      /* shortAddr | panId << 16 */
} rf_src_short_list_t;
typedef struct {
    uint32_t enable[1], pending[1];
    uint32_t entry[2 * RF_SRC_EXT_ENTRIES];    /* 64-bit LE addresses */
} rf_src_ext_list_t;
typedef struct {
    rf_src_short_list_t short_list;
    rf_src_ext_list_t ext_list;
    uint32_t short_used, ext_used;
    uint32_t overflows;
} rf_src_match_t;
#define RF_SRC_NONE 0xFFu
void rf_src_match_init(rf_src_match_t *t);
/* Prepare a change. Returns the entry index to (en|dis)able, or RF_SRC_NONE
 * when nothing must be sent (clearing an absent address) or when the list is
 * full (counted in `overflows`, *full set). With direct=true (RX not
 * running) the enable/pending words are also written here. */
uint8_t rf_src_match_update(rf_src_match_t *t, bool ext, uint64_t address, uint16_t pan_id,
                            bool pending, bool direct, bool *full);
#endif
