#include "core/jit.h"
#include <string.h>

void jit_init(jit_t* j) {
    memset(j, 0, sizeof(*j));
    for (u32 i = 0; i < JIT_MAX_BLOCKS; ++i) j->lookup_idx[i] = -1;
    codegen_init(&j->cg);
}

static int lookup(jit_t* j, u32 pc) {
    /* Direct-mapped 1024-entry hash. */
    u32 h = (pc >> 1) & (JIT_MAX_BLOCKS - 1);
    if (j->lookup_idx[h] >= 0 && j->lookup_pc[h] == pc) return j->lookup_idx[h];
    return -1;
}

static void install(jit_t* j, u32 pc, int idx) {
    u32 h = (pc >> 1) & (JIT_MAX_BLOCKS - 1);
    j->lookup_pc[h]  = pc;
    j->lookup_idx[h] = idx;
}

/* Returns true if op terminates a basic block (branch / exception). */
static bool is_terminator(opcode_t op) {
    switch (op) {
        case OP_B_COND: case OP_B_UNCOND: case OP_BX: case OP_BLX_REG:
        case OP_T32_BL: case OP_T32_B_COND:
        case OP_CBZ: case OP_CBNZ:
        case OP_POP: case OP_T32_LDM:
        case OP_UDF: case OP_SVC: case OP_BKPT:
        case OP_T32_TBB: case OP_T32_TBH:
            return true;
        default: return false;
    }
}

static jit_block_t* compile_block(jit_t* j, bus_t* b, u32 pc) {
    if (j->n_blocks >= JIT_MAX_BLOCKS) {
        /* Cache full: generation reset. All slots wiped; fresh compile from slot 0. */
        jit_flush(j);
    }
    jit_block_t* bk = &j->blocks[j->n_blocks];
    bk->pc_start = pc;
    bk->n_ins = 0;
    bk->native = NULL;
    u32 cur = pc;
    while (bk->n_ins < JIT_MAX_BLOCK_LEN) {
        insn_t ins;
        decode(b, cur, &ins);
        ins.pc = cur;
        bk->ins[bk->n_ins++] = ins;
        cur += ins.size;
        if (is_terminator(ins.op)) break;
    }
    bk->pc_end = cur;
    /* Try native codegen for whole block. */
    bk->native = codegen_emit(&j->cg, b, bk->ins, bk->n_ins);
    install(j, pc, (int)j->n_blocks);
    j->n_blocks++;
    return bk;
}

bool jit_run(jit_t* j, cpu_t* c, bus_t* b, exec_fn execute, u64* out_steps) {
    u32 pc = c->r[REG_PC];
    int idx = lookup(j, pc);
    jit_block_t* bk;
    if (idx < 0) {
        /* Not cached; if hot enough, compile. We track hits on PC tagged
           direct-mapped slots (reuse hash lookup). */
        u32 h = (pc >> 1) & (JIT_MAX_BLOCKS - 1);
        if (j->lookup_idx[h] < 0 || j->lookup_pc[h] != pc) {
            j->lookup_pc[h] = pc;
            j->lookup_idx[h] = -2; /* -2 = counting */
        }
        if (++j->counters[h] < JIT_HOT_THRESHOLD) { *out_steps = 0; return false; }
        bk = compile_block(j, b, pc);
        if (!bk) { *out_steps = 0; return false; }
    } else {
        bk = &j->blocks[idx];
    }
    u64 steps = 0;
    if (bk->native) {
        /* x86 thunk: writes regs + pc directly. Fall back if it fails. */
        if (bk->native(c, b)) {
            steps = bk->n_ins;
            j->native_steps += steps;
        }
    }
    if (steps == 0) {
        /* Interpret pre-decoded ins[]. */
        for (u8 i = 0; i < bk->n_ins; ++i) {
            if (c->halted) break;
            c->r[REG_PC] = bk->ins[i].pc;
            if (!execute(c, b, &bk->ins[i])) break;
            steps++;
            if (is_terminator(bk->ins[i].op)) break;
        }
    }
    j->jit_steps += steps;
    *out_steps = steps;
    return steps > 0;
}

bool jit_run_chained(jit_t* j, cpu_t* c, bus_t* b, exec_fn execute,
                     u64 max_steps, u64* out_steps, const bool* stop) {
    u64 total = 0u;
    while (!c->halted && total < max_steps) {
        u64 remaining = max_steps - total;
        /* Exit early when budget is too tight to avoid overshooting by >31 cycles. */
        if (remaining < (u64)JIT_MAX_BLOCK_LEN) break;
        /* Exit if caller signals a pending exception (e.g. pendsv_pending set
           by firmware ICSR write mid-chain) so caller can dispatch promptly. */
        if (stop && *stop) break;
        u64 steps = 0u;
        if (!jit_run(j, c, b, execute, &steps)) break;
        if (steps == 0u) break;
        total += steps;
    }
    *out_steps = total;
    return total > 0u;
}

void jit_reset_counters(jit_t* g) {
    if (!g) return;
    for (u32 i = 0; i < JIT_MAX_BLOCKS; ++i) g->counters[i] = 0;
}

void jit_flush(jit_t* g) {
    if (!g) return;
    g->n_blocks     = 0u;
    g->lookup_n     = 0;            /* CRITICAL: without this, install() appends at stale pos and lookup scan misses */
    g->jit_steps    = 0u;
    g->native_steps = 0u;
    g->interp_steps = 0u;
    g->cg.used      = 0u;
    for (u32 i = 0; i < JIT_MAX_BLOCKS; ++i) {
        g->lookup_idx[i] = -1;
        g->lookup_pc[i]  = 0u;
        g->counters[i]   = 0u;
    }
}
