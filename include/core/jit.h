#ifndef CORTEX_M_JIT_H
#define CORTEX_M_JIT_H

#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/decoder.h"
#include "core/codegen.h"

/* Basic-block JIT: count how many times each PC starts a block; once a
   threshold is exceeded, compile a sequence of decoded ARM ops into a
   native x86-64 thunk that mutates cpu_t* directly. The "compiled" block
   is currently a tighter dispatch chain (skips re-decode, skips IT/IRQ
   checks) — a portable JIT-lite that gets us most of the speedup without
   needing a full code generator.

   Returns next PC; sets *steps to the number of arm insns executed in
   this block. If the address is not yet hot or block not found, returns
   PC unchanged and *steps=0. */
typedef bool (*exec_fn)(cpu_t*, bus_t*, const insn_t*);

#define JIT_MAX_BLOCKS    1024
#define JIT_MAX_BLOCK_LEN 32
#define JIT_HOT_THRESHOLD 16

typedef struct jit_block_s {
    u32 pc_start;
    u32 pc_end;
    u32 hits;
    u8  n_ins;
    insn_t ins[JIT_MAX_BLOCK_LEN];
    cg_thunk_t native;     /* non-NULL = compiled to x86 machine code */
} jit_block_t;

typedef struct jit_s {
    jit_block_t blocks[JIT_MAX_BLOCKS];
    u32 n_blocks;
    u32 lookup_pc[JIT_MAX_BLOCKS];
    int lookup_idx[JIT_MAX_BLOCKS];
    int lookup_n;
    u64 jit_steps;
    u64 native_steps;
    u64 interp_steps;
    u32 counters[JIT_MAX_BLOCKS]; /* hot-block hit counters */
    codegen_t cg;
} jit_t;

void jit_init(jit_t* j);
/* Try to fast-execute starting at PC. Returns true if at least one insn ran
   via JIT cache; updates *out_steps. */
bool jit_run(jit_t* j, cpu_t* c, bus_t* b, exec_fn execute, u64* out_steps);
void jit_reset_counters(jit_t* g);

#endif
