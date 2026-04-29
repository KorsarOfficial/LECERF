#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/bus.h"
#include "core/decoder.h"
#include "core/nvic.h"
#include "core/run.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "periph/dwt.h"
#include "core/gdb.h"
#include "core/jit.h"
#include "core/tt.h"

static jit_t g_jit;
static int g_jit_inited = 0;

/* Process-globals used by legacy entry points only.
   hot path: run_steps_full_gc reads ctx->dwt/nvic instead. */
dwt_t*  g_dwt_for_run  = NULL;
nvic_t* g_nvic_for_run = NULL;

extern bool execute(cpu_t* c, bus_t* bus, const insn_t* i);

/* Direct-mapped decode cache: 4096 entries × (PC tag + decoded insn).
   Hit rate on hot loops is near 100% — this skips re-decoding entirely. */
#define DCACHE_SIZE 4096
#define DCACHE_MASK (DCACHE_SIZE - 1)

typedef struct { u32 tag; insn_t ins; bool valid; } dcache_e_t;

static dcache_e_t g_dcache[DCACHE_SIZE];

static void dcache_invalidate(void) {
    memset(g_dcache, 0, sizeof(g_dcache));
}

void run_dcache_invalidate(void) { dcache_invalidate(); }

FORCE_INLINE void cache_decode(bus_t* bus, addr_t pc, insn_t* out) {
    u32 idx = (pc >> 1) & DCACHE_MASK;
    dcache_e_t* e = &g_dcache[idx];
    if (LIKELY(e->valid && e->tag == pc)) {
        *out = e->ins;
        return;
    }
    decode(bus, pc, out);
    e->tag = pc;
    e->ins = *out;
    e->valid = true;
}

/* Cap chain budget so the chain returns before missing a pending exception.
   Two caps:
   1. SysTick CVR: chain must yield before the next tick boundary so each
      reload fires as a separate irq_pending event (not silently coalesced).
   2. PendSV pending: context switches (FreeRTOS vTaskDelay/portYIELD) must
      be dispatched promptly; cap to JIT_MAX_BLOCK_LEN so at most one more
      block runs before the scheduler gets control. Without this cap, the chain
      could run rvr=159999 more cycles after portYIELD sets pendsv_pending,
      starving the other task. */
FORCE_INLINE u64 irq_safe_budget(const systick_t* st, const scb_t* scb, u64 remaining) {
    u64 cap = remaining;
    /* PendSV cap: if a context switch is already requested, limit to one block. */
    if (scb && scb->pendsv_pending) {
        u64 psv_cap = (u64)JIT_MAX_BLOCK_LEN;
        if (psv_cap < cap) cap = psv_cap;
    }
    if (!st) return cap;
    if (!(st->csr & 1u)) return cap;   /* SysTick disabled */
    if (!(st->csr & 2u)) return cap;   /* TICKINT disabled -- no IRQ */
    u32 cvr = st->cvr ? st->cvr : st->rvr;
    if (cvr == 0u) return cap;
    u64 stk_cap = (u64)cvr;
    return stk_cap < cap ? stk_cap : cap;
}

/* =====================================================================
   run_steps_full_gc: context-threaded run loop.
   Reads dwt/nvic/tt/jit/replay from ctx; NO g_* reads in hot path.
   ===================================================================== */
u64 run_steps_full_gc(run_ctx_t* ctx, u64 max_steps) {
    if (!ctx) return 0u;
    cpu_t*     c    = ctx->cpu;
    bus_t*     bus  = ctx->bus;
    systick_t* st   = ctx->st;
    scb_t*     scb  = ctx->scb;
    dwt_t*     dwt  = ctx->dwt;
    nvic_t*    nvic = ctx->nvic;
    jit_t*     g    = ctx->jit;

    dcache_invalidate();
    u64 i = 0;
    for (; i < max_steps && !c->halted; ++i) {
        u64 jit_steps = 0;
        u64 budget = irq_safe_budget(st, scb, max_steps - i);
        const bool* stop = scb ? &scb->pendsv_pending : NULL;
        if (g && jit_run_chained(g, c, bus, execute, budget, &jit_steps, stop)
                && jit_steps > 0) {
            if (st) systick_tick(st, (u32)jit_steps);
            if (dwt && (dwt->ctrl & 1u) && (dwt->demcr & (1u << 24)))
                dwt->cyccnt += (u32)jit_steps;
            i += jit_steps - 1;
            goto check_irqs_gc;
        }
        {
            insn_t ins;
            cache_decode(bus, c->r[REG_PC], &ins);
            if (!execute(c, bus, &ins)) break;
        }
        if (st) systick_tick(st, 1);
        if (dwt) dwt_tick(dwt);

    check_irqs_gc:
        if (c->mode == MODE_THREAD && !(c->primask & 1u) && c->basepri == 0) {
            if (st && st->irq_pending) {
                st->irq_pending = false;
                exc_enter(c, bus, EXC_SYSTICK);
                continue;
            }
            if (scb && scb->pendsv_pending) {
                scb->pendsv_pending = false;
                exc_enter(c, bus, EXC_PENDSV);
                continue;
            }
            if (nvic) {
                int irq = nvic_pick(nvic);
                if (irq >= 0) {
                    nvic_clear_pending(nvic, (u32)irq);
                    nvic_set_active(nvic, (u32)irq);
                    exc_enter(c, bus, (u8)(EXC_IRQ0 + irq));
                    continue;
                }
            }
        }
    }
    return i;
}

/* =====================================================================
   GDB-aware run (used by main.c and gdb integration).
   Legacy: still reads g_dwt_for_run / g_nvic_for_run (globals).
   ===================================================================== */
u64 run_steps_full_gdb(cpu_t* c, bus_t* bus, u64 max_steps,
                       systick_t* st, scb_t* scb, gdb_t* gdb) {
    dcache_invalidate();
    if (!g_jit_inited) { jit_init(&g_jit); g_jit_inited = 1; }
    u64 i = 0;
    if (gdb && gdb->active && gdb->halted_for_gdb) {
        gdb_serve(gdb, c, bus);
    }
    for (; i < max_steps && !c->halted; ++i) {
        if (gdb && gdb_should_stop(gdb, c)) {
            gdb->stepping = false;
            gdb->halted_for_gdb = true;
            gdb_serve(gdb, c, bus);
        }
        if (!gdb) {
            u64 jit_steps = 0;
            u64 budget = irq_safe_budget(st, scb, max_steps - i);
            const bool* stop = scb ? &scb->pendsv_pending : NULL;
            if (jit_run_chained(&g_jit, c, bus, execute, budget, &jit_steps, stop) && jit_steps > 0) {
                if (st) systick_tick(st, (u32)jit_steps);
                /* legacy fallback: batch DWT update */
                if (g_dwt_for_run && (g_dwt_for_run->ctrl & 1u) && (g_dwt_for_run->demcr & (1u << 24)))
                    g_dwt_for_run->cyccnt += (u32)jit_steps;
                i += jit_steps - 1;
                goto check_irqs_gdb;
            }
        } else if (gdb && !gdb->stepping) {
            u64 jit_steps = 0;
            u64 budget = irq_safe_budget(st, scb, max_steps - i);
            const bool* stop = scb ? &scb->pendsv_pending : NULL;
            if (jit_run_chained(&g_jit, c, bus, execute, budget, &jit_steps, stop) && jit_steps > 0) {
                if (st) systick_tick(st, (u32)jit_steps);
                if (g_dwt_for_run && (g_dwt_for_run->ctrl & 1u) && (g_dwt_for_run->demcr & (1u << 24)))
                    g_dwt_for_run->cyccnt += (u32)jit_steps;
                i += jit_steps - 1;
                goto check_irqs_gdb;
            }
        }
        {
            insn_t ins;
            cache_decode(bus, c->r[REG_PC], &ins);
            if (!execute(c, bus, &ins)) break;
        }
        if (st) systick_tick(st, 1);
        if (g_dwt_for_run) dwt_tick(g_dwt_for_run);

    check_irqs_gdb:
        if (c->mode == MODE_THREAD && !(c->primask & 1u) && c->basepri == 0) {
            if (st && st->irq_pending) {
                st->irq_pending = false;
                exc_enter(c, bus, EXC_SYSTICK);
                continue;
            }
            if (scb && scb->pendsv_pending) {
                scb->pendsv_pending = false;
                exc_enter(c, bus, EXC_PENDSV);
                continue;
            }
            if (g_nvic_for_run) {
                int irq = nvic_pick(g_nvic_for_run);
                if (irq >= 0) {
                    nvic_clear_pending(g_nvic_for_run, (u32)irq);
                    nvic_set_active(g_nvic_for_run, (u32)irq);
                    exc_enter(c, bus, (u8)(EXC_IRQ0 + irq));
                    continue;
                }
            }
        }
    }
    return i;
}

/* Run with explicit jit_t (TT determinism path: caller owns jit state).
   Legacy wrapper: builds a transient run_ctx_t from globals. */
u64 run_steps_full_g(cpu_t* c, bus_t* bus, u64 max_steps,
                     systick_t* st, scb_t* scb, jit_t* g) {
    run_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cpu    = c;
    ctx.bus    = bus;
    ctx.st     = st;
    ctx.scb    = scb;
    ctx.dwt    = g_dwt_for_run;  /* legacy fallback */
    ctx.nvic   = g_nvic_for_run; /* legacy fallback */
    ctx.jit    = g;
    ctx.tt     = g_tt;           /* legacy fallback */
    ctx.replay = g_replay_mode;  /* legacy fallback */
    return run_steps_full_gc(&ctx, max_steps);
}

u64 run_steps_full(cpu_t* c, bus_t* bus, u64 max_steps,
                   systick_t* st, scb_t* scb) {
    return run_steps_full_gdb(c, bus, max_steps, st, scb, NULL);
}

u64 run_steps_st(cpu_t* c, bus_t* bus, u64 max_steps, systick_t* st) {
    return run_steps_full(c, bus, max_steps, st, NULL);
}

u64 run_steps(cpu_t* c, bus_t* bus, u64 max_steps) {
    return run_steps_full(c, bus, max_steps, NULL, NULL);
}

u64 run_until_cycle(cpu_t* c, bus_t* bus, u64 target,
                    systick_t* st, scb_t* scb, jit_t* g,
                    const ev_t* log, u32 log_n, u32* log_pos,
                    tt_periph_t* p) {
    u64 start = c->cycles;
    while (c->cycles < target) {
        while (log && log_pos && *log_pos < log_n && log[*log_pos].cycle <= c->cycles) {
            tt_inject_event(c, bus, p, &log[*log_pos]);
            (*log_pos)++;
        }
        u64 next_ev = (log && log_pos && *log_pos < log_n) ? log[*log_pos].cycle : target;
        u64 stop    = (next_ev < target) ? next_ev : target;
        u64 gap     = stop - c->cycles;
        if (gap == 0u) break;
        u64 prev = c->cycles;
        run_steps_full_g(c, bus, gap, st, scb, g);
        if (c->cycles == prev) break;  /* watchdog: no forward progress */
    }
    return c->cycles - start;
}
