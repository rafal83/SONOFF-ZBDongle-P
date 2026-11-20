/* SPDX-License-Identifier: MIT
 * The ONLY Flash writer of the FreeRTOS target. Every program/erase is
 * refused outside [HAL_FLASH_STORE_BASE, HAL_FLASH_STORE_END), which lies
 * inside the application area and never overlaps the production Z-Stack NV
 * (0x50000-0x55FFF) or the CCFG page. tools/build_freertos.py rejects any
 * other source file that names a TI Flash write function.
 */
#ifndef FRT_HAL_FLASH_H
#define FRT_HAL_FLASH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAL_FLASH_SECTOR 0x2000u
/* Storage layout 2 (image identity storage_layout = 2): 2 banks x 56 KiB.
 * The application ends below 0x34000 (linker). Layout 1 (2 x 16 KiB at
 * 0x48000, inside the new bank 1) is migrated once at boot (nv_log_migrate). */
#define HAL_FLASH_STORE_BASE 0x34000u
#define HAL_FLASH_STORE_END 0x50000u
#define HAL_FLASH_BANK_SIZE ((HAL_FLASH_STORE_END - HAL_FLASH_STORE_BASE) / 2u)
#define HAL_FLASH_OLD_STORE_BASE 0x48000u
#define HAL_FLASH_OLD_BANK_SIZE 0x4000u

bool hal_flash_read(void *ctx, uint32_t address, void *buffer, size_t length);
bool hal_flash_program(void *ctx, uint32_t address, const void *data, size_t length);
bool hal_flash_erase_sector(void *ctx, uint32_t address);
uint32_t hal_flash_refusals(void);
#endif
