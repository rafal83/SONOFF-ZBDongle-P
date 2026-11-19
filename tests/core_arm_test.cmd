/* Test ELF only. Deliberately no vector table, CCFG or NV regions. */
MEMORY {
    TEST_FLASH (RX) : origin = 0x00001000, length = 0x00020000
    TEST_RAM (RW) : origin = 0x20001800, length = 0x0000e800
}
SECTIONS {
    .text : {} > TEST_FLASH
    .rodata : {} > TEST_FLASH
    .const : {} > TEST_FLASH
    .data : {} > TEST_RAM
    .bss : {} > TEST_RAM
}
