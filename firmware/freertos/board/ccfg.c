/* SPDX-License-Identifier: MIT; the included TI ccfg.c keeps its BSD license.
 * CCFG for the FreeRTOS target. Recovery settings are identical to the
 * production SONOFF images: ROM bootloader enabled, backdoor DIO15 active low,
 * debug access kept, chip/bank erase allowed. tools/image.py check verifies
 * these words in the exported HEX.
 */
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER 15
#define SET_CCFG_BL_CONFIG_BL_LEVEL 0
#define SET_CCFG_CCFG_TAP_DAP_0_CPU_DAP_ENABLE 0xC5
#define SET_CCFG_ERASE_CONF_CHIP_ERASE_DIS_N 1
#define SET_CCFG_ERASE_CONF_BANK_ERASE_DIS_N 1
/* Same clock words as the installed 2026111802 image (MODE_CONF 0xfdb9c1ff):
 * 32.768 kHz XOSC_LF selected for drivers that switch to it, cap-array delta
 * 0xc1 applied. The FreeRTOS boot itself keeps SCLK_LF on RCOSC_LF (reset
 * default) because it never enters standby; see hal_sys.c. */
#define SET_CCFG_MODE_CONF_SCLK_LF_OPTION 2
#define SET_CCFG_MODE_CONF_XOSC_CAP_MOD 0
#define SET_CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA 0xc1
/* Same DC/DC choice as the production launchpad configuration. */
#define SET_CCFG_MODE_CONF_DCDC_RECHARGE 1
#define SET_CCFG_MODE_CONF_DCDC_ACTIVE 1
/* Koenkk/production setting: forced VDDR HH voltage (sets VDDR_EXT_LOAD=0
 * and ALT_DCDC_VMIN=0xC in TI ccfg.c), required by the 14 dBm+ PA entries. */
#define CCFG_FORCE_VDDR_HH 1
#include <startup_files/ccfg.c>
