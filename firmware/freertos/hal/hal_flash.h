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
#define HAL_FLASH_STORE_BASE 0x48000u
#define HAL_FLASH_STORE_END 0x50000u
#define HAL_FLASH_BANK_SIZE ((HAL_FLASH_STORE_END - HAL_FLASH_STORE_BASE) / 2u)

bool hal_flash_read(void *ctx, uint32_t address, void *buffer, size_t length);
bool hal_flash_program(void *ctx, uint32_t address, const void *data, size_t length);
bool hal_flash_erase_sector(void *ctx, uint32_t address);
uint32_t hal_flash_refusals(void);
#endif
