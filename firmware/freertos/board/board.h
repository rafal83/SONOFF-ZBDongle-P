/* SPDX-License-Identifier: MIT
 * SONOFF ZBDongle-P (CC2652P1FRGZ) board description for the FreeRTOS target.
 * Sources: firmware/config/znp-profile.json (Koenkk launchpad profile used in
 * production) and the SysConfig output of znp_CC1352P_2_LAUNCHXL (SDK 8.33).
 */
#ifndef FRT_BOARD_H
#define FRT_BOARD_H

#include <stdint.h>

/* Host link: CP2102N, no RTS/CTS. */
#define BOARD_UART_TX_DIO        13u
#define BOARD_UART_RX_DIO        12u
#ifndef BOARD_UART_BAUD
#define BOARD_UART_BAUD          115200u
#endif

/* ROM serial bootloader backdoor (CCFG), active low. */
#define BOARD_BSL_DIO            15u

/* RF front-end switch (LAUNCHXL-CC1352P-2 compatible wiring).
 * 2.4 GHz standard PA : DIO28 = 1, DIO29 = 0, DIO30 = 0 (GPIO).
 * 2.4 GHz 20 dBm PA   : DIO28 = RFC_GPO0 (LNA), DIO29 = RFC_GPO3 (PA).
 * Radio off           : all low. */
#define BOARD_RF_24GHZ_DIO       28u
#define BOARD_RF_HIGH_PA_DIO     29u
#define BOARD_RF_SUB1GHZ_DIO     30u

/* Flash map. The production Z-Stack NV (NVOCMP) lives in 0x50000-0x55FFF.
 * The FreeRTOS image MUST NOT place code or data there and contains no code
 * path that programs or erases it. */
#define BOARD_FLASH_APP_END      0x00050000u
#define BOARD_FLASH_NV_START     0x00050000u
#define BOARD_FLASH_NV_END       0x00056000u
#define BOARD_FLASH_CCFG         0x00057FA8u
#define BOARD_SRAM_BASE          0x20000000u
#define BOARD_SRAM_SIZE          0x00014000u

/* Interrupt priorities (raw 8-bit values, 3 implemented bits).
 * 0x00 is reserved for code that must never be masked (none today).
 * Every ISR below calls FreeRTOS FromISR APIs, so it must be >= 0x20. */
#define BOARD_IRQ_PRIO_RF        0x20u
#define BOARD_IRQ_PRIO_UART      0x40u
#define BOARD_IRQ_PRIO_WDT       0x20u

#endif
