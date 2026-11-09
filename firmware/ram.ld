ENTRY(_start)
MEMORY {
    /* The same SRAM is mapped to IRAM and DRAM with a 0x700000 offset. */
    drom (r) : ORIGIN = 0x3c030020, LENGTH = 256
    iram (rx) : ORIGIN = 0x40380000, LENGTH = 16K
    dram (rw) : ORIGIN = 0x3fc84000, LENGTH = 288K
}
SECTIONS {
    .flash.appdesc : ALIGN(4) {
        KEEP(*(.app_desc))
    } > drom
    .text : ALIGN(4) {
        KEEP(*(.text.entry))
        *(.text .text.*)
        . = ALIGN(4);
    } > iram
    .rodata : ALIGN(4) {
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(4);
    } > dram
}
