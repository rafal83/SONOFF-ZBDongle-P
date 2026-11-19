/* SPDX-License-Identifier: MIT */
#ifndef FRT_RF_SETTINGS_H
#define FRT_RF_SETTINGS_H
#include <stddef.h>
#include <stdint.h>
#include "rf_logic.h"

/* Mutable: the PA gain words and the segment link are patched at run time. */
extern uint32_t rf_overrides_base[4];
extern uint32_t rf_overrides_tx_std[7];
extern uint32_t rf_overrides_tx20[7];
extern const rf_pa_entry_t rf_pa_table[];
extern const size_t rf_pa_table_count;
#define RF_TX_STD_PATTERN 0x022Bu
#define RF_TX20_PATTERN   0x002Bu
#define RF_TX20_ENABLED   0xFFFFu
#endif
