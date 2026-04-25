#include "periph/uart.h"
#include "core/tt.h"
#include <stdio.h>

static u32 uart_read(void* ctx, addr_t off, u32 size) {
    (void)size;
    uart_t* u = (uart_t*)ctx;
    switch (off) {
        case 0x00: /* DR: pop from rx_q if replay_mode and queue non-empty */
            if (u && u->replay_mode && u->rx_head != u->rx_tail)
                return u->rx_q[u->rx_head++ & 63u];
            return 0;
        case 0x04: /* SR: TXE=1 always; RXNE(bit5) set iff replay queue has data */
            if (u && u->replay_mode && u->rx_head != u->rx_tail)
                return 0x21u; /* TXE|RXNE */
            return 0x1u;     /* TXE only */
        default: return 0;
    }
}

static void uart_write(void* ctx, addr_t off, u32 val, u32 size) {
    (void)size;
    uart_t* u = (uart_t*)ctx;
    if (off == 0x00) {
        if (u && u->replay_mode) return; /* suppress TX in replay */
        int c = (int)(val & 0xFF);
        if (u && u->sink) u->sink(c);
        else { fputc(c, stdout); fflush(stdout); }
    }
}

int uart_attach(bus_t* b, uart_t* u) {
    return bus_add_mmio(b, "uart", UART_BASE, UART_SIZE, u, uart_read, uart_write);
}

void uart_inject_rx(uart_t* u, u8 byte) {
    u->rx_q[u->rx_tail++ & 63u] = byte;
}

/* Stub; 13-04 wires g_tt and provides strong override. */
void uart_record_rx(uart_t* u, u8 byte, u64 cycle) {
    (void)u;
    tt_record_uart_rx(cycle, byte);
}
