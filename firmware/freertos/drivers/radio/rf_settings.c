/* SPDX-License-Identifier: MIT AND BSD-3-Clause
 * Original arrangement under MIT; TI and Koenkk configuration data retain
 * their upstream notices. See NOTICE.md and LICENSES/ in the repository.
 * Register values: TI SysConfig 1.23 RadioConfig 1.20 / SmartRF Studio data
 * 2.32.0 for LAUNCHXL-CC1352P-2, "IEEE 802.15.4-2006, 250 kbps, OQPSK" -
 * exactly the lists linked in the installed Z-Stack image 2026111802
 * (vendor-sdk/.../workspace/znp_CC1352P_2_LAUNCHXL_tirtos7_ticlang/
 * ti_radio_config.c). High-PA entries 6..13 dBm come from the Koenkk patch
 * (MIT) used by that image.
 */
#include "rf_settings.h"
#include <driverlib/rf_mailbox.h>

/* Base list: one spare word so a TX20 segment link can replace the END. */
uint32_t rf_overrides_base[4] = {
    0x000F8883u, /* Rx: LNA bias current offset +15 */
    0x00F388D3u, /* Tx: DCDC IPEAK=3, dither off */
    END_OVERRIDE,
    END_OVERRIDE,
};

uint32_t rf_overrides_tx_std[7] = {
    TX_STD_POWER_OVERRIDE(0x7217),
    0x05320703u,                 /* ANADIV for standard PA */
    0x00008783u,                 /* RTIM offset default */
    0x050206C3u,                 /* synth mux default */
    HW_REG_OVERRIDE(0x60A8, 0x0401), /* TXRX pin: 0 in RX, Hi-Z idle/TX */
    END_OVERRIDE,
    END_OVERRIDE,
};

uint32_t rf_overrides_tx20[7] = {
    TX20_POWER_OVERRIDE(0x003F75F5),
    0x01C20703u,                 /* ANADIV for high PA */
    0x00038783u,                 /* RTIM offset 3 */
    0x010206C3u,                 /* synth mux high PA */
    HW_REG_OVERRIDE(0x60A8, 0x0001), /* TXRX pin: 0 in RX/TX, Hi-Z idle */
    END_OVERRIDE,
    END_OVERRIDE,
};

const rf_pa_entry_t rf_pa_table[] = {
    {-20, false, 0x04C6}, {-18, false, 0x06C8}, {-15, false, 0x06CA},
    {-12, false, 0x0ACC}, {-10, false, 0x0ACF}, {-9, false, 0x0AD0},
    {-6, false, 0x10D4},  {-5, false, 0x12D6},  {-3, false, 0x1893},
    {0, false, 0x2853},   {1, false, 0x2856},   {2, false, 0x3259},
    {3, false, 0x385D},   {4, false, 0x4E63},   {5, false, 0x7217},
    {6, true, 0x144f2a},  {7, true, 0x14285f},  {8, true, 0x10335a},
    {9, true, 0x103f5f},  {10, true, 0x104f66}, {11, true, 0x3021cb},
    {12, true, 0x3f27cc}, {13, true, 0x3fc3cd}, {14, true, 0x1B27D6},
    {15, true, 0x1B2FDA}, {16, true, 0x1B39DE}, {17, true, 0x1B4FE5},
    {18, true, 0x3047E0}, {19, true, 0x3F61E2}, {20, true, 0x3F75F5},
};
const size_t rf_pa_table_count = sizeof rf_pa_table / sizeof rf_pa_table[0];
