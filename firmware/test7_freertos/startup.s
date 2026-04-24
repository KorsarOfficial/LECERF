    .syntax unified
    .cpu cortex-m3
    .thumb

    .section .vectors, "a"
    .word   0x20004000          @ initial MSP (top of 16KB SRAM)
    .word   Reset_Handler + 1
    .word   default_handler + 1 @ NMI
    .word   hard_fault + 1      @ HardFault
    .word   default_handler + 1
    .word   default_handler + 1
    .word   default_handler + 1
    .word   default_handler + 1
    .word   default_handler + 1
    .word   default_handler + 1
    .word   default_handler + 1
    .word   SVC_Handler + 1     @ [11] SVC
    .word   default_handler + 1
    .word   default_handler + 1
    .word   PendSV_Handler + 1  @ [14] PendSV
    .word   SysTick_Handler + 1 @ [15] SysTick

    .text
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    ldr     r0, =__data_start
    ldr     r1, =__data_end
    ldr     r2, =__data_load
1:  cmp     r0, r1
    beq     2f
    ldr     r3, [r2]
    str     r3, [r0]
    adds    r0, #4
    adds    r2, #4
    b       1b
2:
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r3, #0
3:  cmp     r0, r1
    beq     4f
    str     r3, [r0]
    adds    r0, #4
    b       3b
4:
    bl      main
5:  b       5b

    .thumb_func
    .global default_handler
default_handler:
1:  b       1b

    .thumb_func
    .global hard_fault
hard_fault:
    .short  0xDEFE
