MEMORY {
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 64K
    SRAM  (rw) : ORIGIN = 0x20000000, LENGTH = 16K
}

ENTRY(reset_handler)

SECTIONS {
    .vectors : { KEEP(*(.vectors)) } > FLASH
    .text    : { *(.text*) *(.rodata*) } > FLASH

    .data : {
        __data_start = .;
        *(.data*)
        __data_end = .;
    } > SRAM AT > FLASH
    __data_load = LOADADDR(.data);

    .bss : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        __bss_end = .;
    } > SRAM
}
