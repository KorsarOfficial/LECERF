#include "lecerf.h"
#include "core/board.h"
#include <stdint.h>

/* Thin extern "C" forwards from lecerf.h API to board.c implementation.
   The lecerf_board_t handle is just a board_t*. */

lecerf_board_t lecerf_board_create(const char* n) {
    return board_create(n);
}

void lecerf_board_destroy(lecerf_board_t b) {
    board_destroy((board_t*)b);
}

int lecerf_board_flash(lecerf_board_t b, const uint8_t* data, uint32_t sz) {
    return board_flash((board_t*)b, data, sz) ? 1 : 0;
}

uint64_t lecerf_board_run(lecerf_board_t b, uint64_t max_steps, int* exit_cause) {
    return board_run((board_t*)b, max_steps, exit_cause);
}

uint32_t lecerf_board_uart_drain(lecerf_board_t b, uint8_t* dst, uint32_t cap) {
    return board_uart_drain((board_t*)b, dst, cap);
}

int lecerf_board_gpio_get(lecerf_board_t b, uint32_t port, uint32_t pin) {
    return board_gpio_get((const board_t*)b, port, pin);
}

uint32_t lecerf_board_cpu_reg(lecerf_board_t b, uint32_t n) {
    return board_cpu_reg((const board_t*)b, n);
}

uint64_t lecerf_board_cycles(lecerf_board_t b) {
    return board_cycles((const board_t*)b);
}

void lecerf_board_enable_timetravel(lecerf_board_t b, uint32_t stride,
                                    uint32_t max_snaps) {
    board_enable_timetravel((board_t*)b, stride, max_snaps);
}

void lecerf_board_inject_irq(lecerf_board_t b, uint32_t irq) {
    board_inject_irq((board_t*)b, irq);
}

uint32_t lecerf_board_get_ev_log_count(lecerf_board_t b) {
    return board_get_ev_log_count((const board_t*)b);
}
