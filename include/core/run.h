#ifndef CORTEX_M_RUN_H
#define CORTEX_M_RUN_H

#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "core/jit.h"

/* Run with explicit jit instance (for TT determinism: caller owns jit_t). */
u64 run_steps_full_g(cpu_t* c, bus_t* bus, u64 max_steps,
                     systick_t* st, scb_t* scb, jit_t* g);
u64 run_steps_full  (cpu_t* c, bus_t* bus, u64 max_steps,
                     systick_t* st, scb_t* scb);
u64 run_steps_st    (cpu_t* c, bus_t* bus, u64 max_steps, systick_t* st);
u64 run_steps       (cpu_t* c, bus_t* bus, u64 max_steps);
void run_dcache_invalidate(void);

/* Forward decls for replay engine types (full defs in core/tt.h). */
struct ev_s;
struct tt_periph_s;

/* run_until_cycle: loops run_steps_full_g, drains log events at cycle stamps,
   stops when c->cycles >= target_cycle. Overshoot <= one ARM cycle. */
u64 run_until_cycle(cpu_t* c, bus_t* bus, u64 target_cycle,
                    systick_t* st, scb_t* scb, jit_t* g,
                    const struct ev_s* log, u32 log_n, u32* log_pos,
                    struct tt_periph_s* p);

#endif
