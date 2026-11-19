/* SPDX-License-Identifier: MIT
 * Test-only layout for executing portable modules in Unicorn. No vector
 * table, no CCFG: this ELF is never exported to HEX and never flashed.
 */
ENTRY(aes128_init)
MEMORY {
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x00050000
    SRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x00060000   /* larger than the MCU: test-only */
}
SECTIONS {
    .text : { *(.text*) *(.rodata*) } > FLASH
    .data : { _data = .; *(.data*) _edata = .; } > SRAM AT > FLASH
    _ldata = LOADADDR(.data);
    .bss (NOLOAD) : { _bss = .; *(.bss*) *(COMMON) _ebss = .; } > SRAM
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
