#include "periph/stm32.h"
#include <stdio.h>

/* === RCC === */
static u32 rcc_read(void* ctx, addr_t off, u32 size) {
    (void)ctx; (void)size;
    /* CR (+0x00): bit 17 HSERDY = 1, bit 25 PLLRDY = 1 — ack any config. */
    if (off == 0x00) return (1u << 17) | (1u << 25) | 0x83u;
    /* CFGR (+0x04): SWS reflects SW. */
    return 0;
}
static void rcc_write(void* ctx, addr_t off, u32 v, u32 size) {
    (void)ctx; (void)off; (void)v; (void)size;
}

/* === GPIO === */
typedef struct { stm32_t* s; char tag; } gpio_ctx_t;
static gpio_ctx_t g_a, g_b, g_c;

static u32 gpio_read(void* ctx, addr_t off, u32 size) {
    (void)size;
    gpio_ctx_t* g = (gpio_ctx_t*)ctx;
    if (off == 0x0C) { /* IDR */
        return 0;
    }
    if (off == 0x0C + 4) { /* ODR */
        if (g->tag == 'A') return g->s->odr_a;
        if (g->tag == 'B') return g->s->odr_b;
        return g->s->odr_c;
    }
    return 0;
}
static void gpio_write(void* ctx, addr_t off, u32 v, u32 size) {
    (void)size;
    gpio_ctx_t* g = (gpio_ctx_t*)ctx;
    u32* odr = (g->tag == 'A') ? &g->s->odr_a :
               (g->tag == 'B') ? &g->s->odr_b : &g->s->odr_c;
    if (off == 0x0C + 4) { /* ODR */
        *odr = v;
        if (!g->s->quiet) fprintf(stderr, "[GPIO%c] ODR=0x%04x\n", g->tag, v & 0xFFFF);
    } else if (off == 0x10) { /* BSRR — set/reset */
        u32 set = v & 0xFFFF;
        u32 rst = (v >> 16) & 0xFFFF;
        *odr = (*odr | set) & ~rst;
        if (!g->s->quiet) fprintf(stderr, "[GPIO%c] BSRR set=0x%04x rst=0x%04x ODR=0x%04x\n",
                                  g->tag, set, rst, *odr & 0xFFFF);
    } else if (off == 0x14) { /* BRR — reset only */
        *odr &= ~(v & 0xFFFF);
    }
}

/* === USART === */
static u32 usart_read(void* ctx, addr_t off, u32 size) {
    (void)ctx; (void)size;
    if (off == 0x00) return 0xC0; /* SR: TXE | TC always */
    return 0;
}
static void usart_write(void* ctx, addr_t off, u32 v, u32 size) {
    (void)ctx; (void)size;
    if (off == 0x04) { /* DR: TX byte */
        fputc((int)(v & 0xFF), stdout);
        fflush(stdout);
    }
}

int stm32_attach(bus_t* b, stm32_t* s) {
    *s = (stm32_t){0};
    g_a = (gpio_ctx_t){s, 'A'};
    g_b = (gpio_ctx_t){s, 'B'};
    g_c = (gpio_ctx_t){s, 'C'};
    bus_add_mmio(b, "rcc",    STM32_RCC_BASE,    0x400, NULL, rcc_read, rcc_write);
    bus_add_mmio(b, "gpioa",  STM32_GPIOA_BASE,  0x400, &g_a, gpio_read, gpio_write);
    bus_add_mmio(b, "gpiob",  STM32_GPIOB_BASE,  0x400, &g_b, gpio_read, gpio_write);
    bus_add_mmio(b, "gpioc",  STM32_GPIOC_BASE,  0x400, &g_c, gpio_read, gpio_write);
    bus_add_mmio(b, "usart1", STM32_USART1_BASE, 0x400, NULL, usart_read, usart_write);
    bus_add_mmio(b, "usart2", STM32_USART2_BASE, 0x400, NULL, usart_read, usart_write);
    return 0;
}
