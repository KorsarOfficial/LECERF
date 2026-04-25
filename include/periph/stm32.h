#ifndef CORTEX_M_STM32_H
#define CORTEX_M_STM32_H

#include "core/types.h"
#include "core/bus.h"

/* Minimal STM32F103 (Blue Pill) periphery for emulator runs.
   - RCC at 0x40021000: clocks (we accept any config, ack ready bits)
   - GPIOA/B/C at 0x40010800/0x40010C00/0x40011000: writes go to stdout as
     "[GPIO<port>] OUT=0x<bits>\n" so firmware can show LED toggles
   - USART1 at 0x40013800, USART2 at 0x40004400: TX byte → stdout */

#define STM32_RCC_BASE     0x40021000u
#define STM32_GPIOA_BASE   0x40010800u
#define STM32_GPIOB_BASE   0x40010C00u
#define STM32_GPIOC_BASE   0x40011000u
#define STM32_USART1_BASE  0x40013800u
#define STM32_USART2_BASE  0x40004400u

typedef struct {
    u32 odr_a, odr_b, odr_c;
    bool quiet;
} stm32_t;

int stm32_attach(bus_t* b, stm32_t* s);

#endif
