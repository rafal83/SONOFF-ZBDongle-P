/* SPDX-License-Identifier: MIT; included TI ccfg.c retains its BSD license. */
/* UART-only laboratory build. RF core is never powered. */
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER 15
#define SET_CCFG_BL_CONFIG_BL_LEVEL 0
#define SET_CCFG_CCFG_TAP_DAP_0_CPU_DAP_ENABLE 0xC5
#define SET_CCFG_ERASE_CONF_CHIP_ERASE_DIS_N 1
#define SET_CCFG_ERASE_CONF_BANK_ERASE_DIS_N 1
/* Diagnostic uses RC LF so no unverified board LF crystal is required. */
#define SET_CCFG_MODE_CONF_SCLK_LF_OPTION 3
#define SET_CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA 0xc1
/* Upstream launchpad generated configuration disables DCDC. */
#define SET_CCFG_MODE_CONF_DCDC_RECHARGE 1
#define SET_CCFG_MODE_CONF_DCDC_ACTIVE 1
#include <startup_files/ccfg.c>
