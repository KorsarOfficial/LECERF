#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"

GCC="/c/Program Files (x86)/Arm/GNU Toolchain mingw-w64-i686-arm-none-eabi/bin/arm-none-eabi-gcc.exe"
OBJCOPY="/c/Program Files (x86)/Arm/GNU Toolchain mingw-w64-i686-arm-none-eabi/bin/arm-none-eabi-objcopy.exe"

CFLAGS="-mcpu=cortex-m3 -mthumb -O2 -ffreestanding -nostdlib -fno-exceptions -fno-stack-protector -Wall"
LDFLAGS="-Wl,--gc-sections -T link.ld -nostartfiles"

"$GCC" $CFLAGS -c startup.s -o startup.o
"$GCC" $CFLAGS -c main.c    -o main.o
"$GCC" $CFLAGS $LDFLAGS startup.o main.o -o test10_stm32_blink.elf
"$OBJCOPY" -O binary test10_stm32_blink.elf test10_stm32_blink.bin
ls -la test10_stm32_blink.bin
