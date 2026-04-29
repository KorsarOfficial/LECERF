#ifndef CORTEX_M_UART_H
#define CORTEX_M_UART_H

#include "core/types.h"
#include "core/bus.h"

/* Minimal UART at 0x40004000:
   +0x00 DR   : write = TX byte, read = RX byte (0 if empty)
   +0x04 SR   : bit 0 TXE (always 1), bit 5 RXNE (set when replay queue non-empty)
   +0x08 CR   : control (ignored for now) */

#define UART_BASE 0x40004000u
#define UART_SIZE 0x1000u

typedef struct uart_s {
    int (*sink)(void* ctx, int c); /* TX sink: ctx=sink_ctx, c=byte; NULL -> fputc(stdout) */
    void* sink_ctx;                /* first arg passed to sink */
    bool replay_mode;              /* true -> suppress TX; RX from rx_q */
    u8   rx_q[64];                 /* circular RX byte queue (power-of-2 size) */
    u8   rx_head;
    u8   rx_tail;
} uart_t;

int  uart_attach(bus_t* b, uart_t* u);
void uart_inject_rx   (uart_t* u, u8 byte);             /* push byte into rx_q */
void uart_record_rx   (uart_t* u, u8 byte, u64 cycle);  /* record hook (stub; 13-04 wires g_tt) */

#endif
