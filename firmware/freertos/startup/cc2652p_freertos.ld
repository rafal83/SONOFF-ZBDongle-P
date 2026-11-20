/* SPDX-License-Identifier: MIT
 * CC2652P1F memory map for the FreeRTOS target (LLD / GNU ld syntax).
 * FLASH_APP stops at 0x48000: the store, NV (0x50000-0x55FFF) and the
 * spare page are never part of this image. CCFG is the last 88 bytes.
 */
ENTRY(Reset_Handler)
MEMORY {
    FLASH_APP (rx) : ORIGIN = 0x00000000, LENGTH = 0x00034000   /* 0x34000-0x4FFFF: nv_log store (layout 2) */
    CCFG (r)       : ORIGIN = 0x00057FA8, LENGTH = 88
    SRAM (rwx)     : ORIGIN = 0x20000000, LENGTH = 0x00014000
}
/* MSP stack used by exceptions and by main() before the scheduler starts. */
__msp_stack_size = 0x800;

SECTIONS {
    .text : ALIGN(4) {
        KEEP(*(.resetVecs))
        . = 0x100;
        KEEP(*(.imageInfo))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH_APP
    .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > FLASH_APP
    .data : ALIGN(4) {
        _data = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH_APP
    _ldata = LOADADDR(.data);
    .bss (NOLOAD) : ALIGN(8) {
        _bss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > SRAM
    /* Survives software/watchdog resets; validated by checksum before use. */
    .noinit (NOLOAD) : ALIGN(4) {
        *(.noinit*)
        . = ALIGN(8);
    } > SRAM
    .stack (NOLOAD) : ALIGN(8) {
        _sstack = .;
        . = . + __msp_stack_size;
        _estack = .;
    } > SRAM
    .ccfg : { KEEP(*(.ccfg)) } > CCFG
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) }
}
_flash_used = LOADADDR(.data) + SIZEOF(.data);
_sram_used = _estack - ORIGIN(SRAM);
ASSERT(_flash_used <= 0x34000, "Image would overlap the nv_log store (layout 2) / production NV region")
ASSERT(SIZEOF(.ccfg) == 88, "Missing or wrong CC26x2 CCFG")
ASSERT(image_info == 0x100, "Wrong application identity address")
ASSERT(_estack <= ORIGIN(SRAM) + LENGTH(SRAM), "SRAM overflow")
