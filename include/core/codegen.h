#ifndef CORTEX_M_CODEGEN_H
#define CORTEX_M_CODEGEN_H

#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/decoder.h"

/* arm-op[] -> x86-64 thunk.
   WIN64 entry: rcx=cpu_t*, rdx=bus_t*.
   Thunk prologue saves both into non-volatile r15/r14 once and uses
   [r15 + R_OFF + r*4] for register access throughout the block. */

#define CG_R_OFF    ((u32)offsetof(cpu_t, r))         /* r[0]..r[15] base */
#define CG_PC_OFF   (CG_R_OFF + 15u * 4u)             /* r[15] (PC) */
#define CG_APSR_OFF ((u32)offsetof(cpu_t, apsr))      /* APSR for flag setters */
#define CG_HALT_OFF ((u32)offsetof(cpu_t, halted))    /* halted flag (1B) */

#define CG_PAGE_SIZE   (64 * 1024)
#define CG_TOTAL_PAGES 32
#define CG_BUFFER_SIZE (CG_PAGE_SIZE * CG_TOTAL_PAGES)

typedef struct codegen_s {
    u8*  buffer;
    u32  used;
    u32  capacity;
} codegen_t;

typedef bool (*cg_thunk_t)(cpu_t* c, bus_t* b);

bool codegen_init(codegen_t* cg);
void codegen_free(codegen_t* cg);
cg_thunk_t codegen_emit(codegen_t* cg, bus_t* b, const insn_t* ins, u8 n);
bool codegen_supports(opcode_t op);

#endif
