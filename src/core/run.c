#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/cpu.h"
#include "core/bus.h"
#include "core/decoder.h"
#include "core/nvic.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "periph/dwt.h"
#include "core/gdb.h"

extern dwt_t* g_dwt_for_run;
dwt_t* g_dwt_for_run = NULL;

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

u64 run_steps_full_g(cpu_t* c, bus_t* bus, u64 max_steps,
                     systick_t* st, scb_t* scb, gdb_t* gdb) {
    dcache_invalidate();
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
        insn_t ins;
        cache_decode(bus, c->r[REG_PC], &ins);
        if (!execute(c, bus, &ins)) break;

        if (st) systick_tick(st, 1);
        if (g_dwt_for_run) dwt_tick(g_dwt_for_run);

        if (c->mode == MODE_THREAD && !(c->primask & 1u)) {
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
        }
    }
    return i;
}

u64 run_steps_full(cpu_t* c, bus_t* bus, u64 max_steps,
                   systick_t* st, scb_t* scb) {
    return run_steps_full_g(c, bus, max_steps, st, scb, NULL);
}

u64 run_steps_st(cpu_t* c, bus_t* bus, u64 max_steps, systick_t* st) {
    return run_steps_full(c, bus, max_steps, st, NULL);
}

u64 run_steps(cpu_t* c, bus_t* bus, u64 max_steps) {
    return run_steps_full(c, bus, max_steps, NULL, NULL);
}
