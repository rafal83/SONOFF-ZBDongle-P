/* SPDX-License-Identifier: MIT
 * Flash writes through TI driverlib (ROM HAPI). The flash cache (VIMS) is
 * disabled around each operation as TI's NVS driver does. While an erase
 * runs (milliseconds) the CPU stalls on flash fetches, including ISRs: UART
 * bytes may be lost then (counted as hardware overruns). Erases only happen
 * during log compaction.
 */
#include "hal_flash.h"

#include <inc/hw_memmap.h>
#include <inc/hw_types.h>
#include <driverlib/flash.h>
#include <driverlib/vims.h>

static volatile uint32_t refusals;

static bool in_store(uint32_t address, size_t length) {
    return address >= HAL_FLASH_STORE_BASE && length <= HAL_FLASH_STORE_END - HAL_FLASH_STORE_BASE &&
           address + length <= HAL_FLASH_STORE_END;
}

uint32_t hal_flash_refusals(void) { return refusals; }

bool hal_flash_read(void *ctx, uint32_t address, void *buffer, size_t length) {
    (void)ctx;
    if (!in_store(address, length)) { refusals++; return false; }
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)address;
    uint8_t *dst = buffer;
    for (size_t i = 0; i < length; i++) dst[i] = src[i];
    return true;
}

static uint32_t cache_off(void) {
    uint32_t mode = VIMSModeGet(VIMS_BASE);
    if (mode != VIMS_MODE_DISABLED) VIMSModeSafeSet(VIMS_BASE, VIMS_MODE_DISABLED, true);
    return mode;
}

static void cache_restore(uint32_t mode) {
    if (mode != VIMS_MODE_DISABLED) VIMSModeSet(VIMS_BASE, mode);
}

bool hal_flash_program(void *ctx, uint32_t address, const void *data, size_t length) {
    (void)ctx;
    if (!length) return true;
    if (!in_store(address, length)) { refusals++; return false; }
    uint32_t mode = cache_off();
    uint32_t status = FlashProgram((uint8_t *)(uintptr_t)data, address, (uint32_t)length);
    cache_restore(mode);
    return status == FAPI_STATUS_SUCCESS;
}

bool hal_flash_erase_sector(void *ctx, uint32_t address) {
    (void)ctx;
    if (address % HAL_FLASH_SECTOR || !in_store(address, HAL_FLASH_SECTOR)) { refusals++; return false; }
    uint32_t mode = cache_off();
    uint32_t status = FlashSectorErase(address);
    cache_restore(mode);
    return status == FAPI_STATUS_SUCCESS;
}
