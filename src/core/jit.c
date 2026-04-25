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
    if (j->n_blocks >= JIT_MAX_BLOCKS) return NULL;
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
    bk->native = codegen_emit(&j->cg, bk->ins, bk->n_ins);
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

void jit_reset_counters(jit_t* g) {
    if (!g) return;
    for (u32 i = 0; i < JIT_MAX_BLOCKS; ++i) g->counters[i] = 0;
}
