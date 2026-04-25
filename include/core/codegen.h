#ifndef CORTEX_M_CODEGEN_H
#define CORTEX_M_CODEGEN_H

#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/decoder.h"

/* arm-op[] -> x86-64 thunk. ABI: rdi=cpu, rsi=bus -> bool. */

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
cg_thunk_t codegen_emit(codegen_t* cg, const insn_t* ins, u8 n);
bool codegen_supports(opcode_t op);

#endif
