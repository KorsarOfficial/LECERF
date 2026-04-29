#ifndef LECERF_H
#define LECERF_H

/* lecerf.h — stable C ABI for the Cortex-M emulator.
   Opaque board handle; safe to load via ctypes / JNI / Python cffi.
   All functions return NULL / 0 on invalid input (no abort). */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Opaque board handle. */
typedef void* lecerf_board_t;

/* Exit cause codes returned via *exit_cause in lecerf_board_run(). */
#define LECERF_HALT    0
#define LECERF_TIMEOUT 1
#define LECERF_FAULT   2

/* Create a board by profile name ("stm32f103", "stm32f407", "generic-m4").
   Returns NULL if name is unknown or OOM. */
lecerf_board_t lecerf_board_create(const char* name);

/* Destroy a board and free all resources. */
void lecerf_board_destroy(lecerf_board_t b);

/* Load firmware blob into flash. Returns 1 on success, 0 on error. */
int lecerf_board_flash(lecerf_board_t b, const uint8_t* data, uint32_t sz);

/* Run up to max_steps instructions.
   *exit_cause is set to LECERF_HALT / LECERF_TIMEOUT / LECERF_FAULT.
   Returns instructions executed. */
uint64_t lecerf_board_run(lecerf_board_t b, uint64_t max_steps, int* exit_cause);

/* Drain UART TX buffer; returns bytes copied, resets internal buffer. */
uint32_t lecerf_board_uart_drain(lecerf_board_t b, uint8_t* dst, uint32_t cap);

/* GPIO read: port 0=A 1=B 2=C, pin 0-15. Returns 0 or 1. */
int lecerf_board_gpio_get(lecerf_board_t b, uint32_t port, uint32_t pin);

/* CPU register: n 0-15 = R0-R15; 16 = APSR. */
uint32_t lecerf_board_cpu_reg(lecerf_board_t b, uint32_t n);

/* Cycle counter. */
uint64_t lecerf_board_cycles(lecerf_board_t b);

/* Enable time-travel (per-board tt_t, does not touch g_tt global). */
void lecerf_board_enable_timetravel(lecerf_board_t b, uint32_t stride,
                                    uint32_t max_snaps);

/* Inject an IRQ (0-based EXC_IRQ0-relative). Records TT event if TT enabled. */
void lecerf_board_inject_irq(lecerf_board_t b, uint32_t irq);

/* Number of events in this board's TT event log; 0 if TT not enabled. */
uint32_t lecerf_board_get_ev_log_count(lecerf_board_t b);

#ifdef __cplusplus
}
#endif

#endif /* LECERF_H */
