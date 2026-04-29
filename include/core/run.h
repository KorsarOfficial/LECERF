#ifndef CORTEX_M_RUN_H
#define CORTEX_M_RUN_H

#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "periph/dwt.h"
#include "core/nvic.h"
#include "core/jit.h"

/* Forward declaration; full definition in core/tt.h */
struct tt_s;

/* run_ctx_t: per-board execution context.
   Threads all 6 formerly-global pointers through the run loop so that
   two board instances can execute concurrently without state cross-talk. */
typedef struct run_ctx_s {
    cpu_t*     cpu;
    bus_t*     bus;
    nvic_t*    nvic;
    systick_t* st;
    scb_t*     scb;
    dwt_t*     dwt;
    jit_t*     jit;
    struct tt_s* tt;    /* per-board TT, may be NULL */
    bool       replay;  /* g_replay_mode replacement, default false */
} run_ctx_t;

/* Context-threaded run loop (no g_* reads in hot path).
   board_run builds a run_ctx_t on the stack and calls this. */
u64 run_steps_full_gc(run_ctx_t* ctx, u64 max_steps);

/* Legacy entry points: preserved so all 19 prior ctest + 14 firmware still pass.
   They build a transient run_ctx_t from the process globals and forward. */
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
