/* SPDX-License-Identifier: MIT; symbols match TI startup_gcc.c */
ENTRY(ResetISR)
MEMORY {
    FLASH (rx) : ORIGIN = 0, LENGTH = 0x50000
    CCFG (r) : ORIGIN = 0x57fa8, LENGTH = 88
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x14000
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
    .text : { KEEP(*(.resetVecs)) *(.text*) *(.rodata*) } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    .data : ALIGN(4) { _data = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
    _ldata = LOADADDR(.data);
    .bss (NOLOAD) : ALIGN(4) { _bss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
    .ccfg : { KEEP(*(.ccfg)) } > CCFG
    /DISCARD/ : { *(.comment) *(.note*) }
}
ASSERT(_ebss + 0x1000 <= _estack, "Less than 4 KiB stack remains")
ASSERT(SIZEOF(.ccfg) == 88, "Missing or wrong CC26x2 CCFG")
