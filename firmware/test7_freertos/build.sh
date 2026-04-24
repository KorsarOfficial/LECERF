#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"

GCC="/c/Program Files (x86)/Arm/GNU Toolchain mingw-w64-i686-arm-none-eabi/bin/arm-none-eabi-gcc.exe"
OBJCOPY="/c/Program Files (x86)/Arm/GNU Toolchain mingw-w64-i686-arm-none-eabi/bin/arm-none-eabi-objcopy.exe"

K="FreeRTOS/Kernel"

CFLAGS="-mcpu=cortex-m3 -mthumb -Os -ffreestanding -nostdlib \
        -fno-exceptions -fno-stack-protector -Wall \
        -I. -I${K}/include -I${K}/portable/GCC/ARM_CM3"
LDFLAGS="-Wl,--gc-sections -T link.ld -nostartfiles"

SRCS="startup.s main.c minlibc.c \
      ${K}/tasks.c ${K}/list.c ${K}/queue.c \
      ${K}/portable/GCC/ARM_CM3/port.c \
      ${K}/portable/MemMang/heap_4.c"

"$GCC" $CFLAGS $LDFLAGS $SRCS -o test7_freertos.elf
"$OBJCOPY" -O binary test7_freertos.elf test7_freertos.bin

ls -la test7_freertos.bin
