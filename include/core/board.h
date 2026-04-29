#ifndef CORTEX_M_BOARD_H
#define CORTEX_M_BOARD_H

#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/jit.h"
#include "core/tt.h"
#include "periph/uart.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "periph/mpu.h"
#include "periph/stm32.h"
#include "periph/dwt.h"
#include "periph/eth.h"
#include "core/nvic.h"

/* Core variant enum is in cpu.h (core_t). */

/* board_profile_t: static description of a supported board. */
typedef struct board_profile_s {
    const char* name;
    addr_t flash_base;
    u32    flash_size;
    addr_t sram_base;
    u32    sram_size;
    core_t core;
    u32    periph_flags;  /* reserved, 0 for now */
} board_profile_t;

/* board_t: owns all peripheral state for one emulated MCU instance.
   Heap-allocated; never embed in another struct (jit alone is ~2 MB). */
typedef struct board_s {
    cpu_t     cpu;
    bus_t     bus;
    jit_t*    jit;          /* heap; ~2 MB, do not embed */
    tt_t*     tt;           /* optional; NULL when TT disabled */
    uart_t    uart;
    systick_t st;
    scb_t     scb;
    mpu_t     mpu;
    stm32_t   stm32;
    nvic_t    nvic;
    dwt_t     dwt;
    eth_t     eth;
    u8*       uart_buf;     /* heap; captures TX bytes */
    u32       uart_buf_n;
    u32       uart_buf_cap;
    const board_profile_t* prof;
} board_t;

/* Lookup a profile by name; NULL if not found. */
const board_profile_t* board_profile_find(const char* name);

/* Lifecycle. */
board_t* board_create(const char* name);
void     board_destroy(board_t* b);

/* Load firmware blob into flash region. */
bool board_flash(board_t* b, const u8* data, u32 sz);

/* Run up to max_steps ARM instructions.
   *exit_cause set to LECERF_HALT / LECERF_TIMEOUT / LECERF_FAULT.
   Returns number of instructions executed. */
#define BOARD_HALT    0
#define BOARD_TIMEOUT 1
#define BOARD_FAULT   2

u64 board_run(board_t* b, u64 max_steps, int* exit_cause);

/* Drain accumulated UART TX bytes; returns bytes copied. Resets internal buffer. */
u32 board_uart_drain(board_t* b, u8* dst, u32 cap);

/* GPIO ODR read: port 0=A, 1=B, 2=C; pin 0-15. Returns 0 or 1. */
int board_gpio_get(const board_t* b, u32 port, u32 pin);

/* CPU register access: n 0-15 = R0-R15; 16 = APSR. */
u32 board_cpu_reg(const board_t* b, u32 n);

/* Cycle counter. */
u64 board_cycles(const board_t* b);

/* Enable time-travel for this board.
   Allocates a per-board tt_t (does NOT touch g_tt). */
void board_enable_timetravel(board_t* b, u32 stride, u32 max_snaps);

/* Inject an IRQ (external test hook); IRQ number is EXC_IRQ0-relative
   (e.g. 0 = IRQ0). Records a TT event on this board's tt if enabled.
   board_run() or board_inject_irq is not required to execute a full run step --
   it only marks the NVIC pending and (if tt) logs the event. */
void board_inject_irq(board_t* b, u32 irq);

/* Return the number of events recorded in this board's TT event log.
   Returns 0 if TT is not enabled. */
u32 board_get_ev_log_count(const board_t* b);

/* Count events in board B's log (used by isolation tests). */
static inline u32 board_ev_log_count(const board_t* b) {
    return b && b->tt ? b->tt->log.n : 0u;
}

#endif
