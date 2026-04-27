#include "test_harness.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/codegen.h"
#include "core/decoder.h"
#include "core/jit.h"
#include <string.h>

static jit_t s_jit;

extern bool execute(cpu_t* c, bus_t* bus, const insn_t* i);

/* Run native thunk and interpreter for one insn; compare r[rd] and apsr. */
static void run_pair(opcode_t op, u32 a, u32 b, u8 sf,
                     bool reg_form, u32 imm, const char* tag) {
    bus_t bus; bus_init(&bus);
    bus_add_flat(&bus, "flash", 0x00000000u, 4096u, false);
    bus_add_flat(&bus, "sram",  0x20000000u, 4096u, true);

    cpu_t c1, c2;
    memset(&c1, 0, sizeof c1); memset(&c2, 0, sizeof c2);
    cpu_reset(&c1, CORE_M4); cpu_reset(&c2, CORE_M4);
    c1.r[1] = a; c1.r[2] = b; c1.r[REG_PC] = 0u; c1.apsr = 0u;
    c2.r[1] = a; c2.r[2] = b; c2.r[REG_PC] = 0u; c2.apsr = 0u;

    insn_t ins; memset(&ins, 0, sizeof ins);
    ins.op = op; ins.rd = 0u; ins.rn = 1u; ins.rm = 2u;
    ins.imm = imm; ins.set_flags = sf ? 1 : 0;
    ins.pc = 0u; ins.size = (op >= OP_T32_BL) ? 4u : 2u;

    /* native */
    memset(&s_jit, 0, sizeof s_jit);
    jit_init(&s_jit);
    cg_thunk_t fn = codegen_emit(&s_jit.cg, &bus, &ins, 1u);
    if (fn) (void)fn(&c1, &bus);

    /* interpreter */
    (void)execute(&c2, &bus, &ins);

    if (c1.apsr != c2.apsr || c1.r[0] != c2.r[0]) {
        fprintf(stderr, "MISMATCH %s op=%d a=%08x b=%08x: "
                        "native r0=%08x apsr=%08x  interp r0=%08x apsr=%08x\n",
                tag, (int)op, a, b, c1.r[0], c1.apsr, c2.r[0], c2.apsr);
    }
    _g_tests++;
    if (c1.apsr != c2.apsr) { fprintf(stderr, "FAIL apsr %s\n", tag); _g_fail++; }
    _g_tests++;
    if (c1.r[0] != c2.r[0]) { fprintf(stderr, "FAIL r0 %s\n", tag); _g_fail++; }
}

int main(void) {
    /* T1 ADD_REG always sets NZCV */
    run_pair(OP_ADD_REG, 0x7FFFFFFFu, 1u,        1, true,  0u, "ADD_REG V");
    run_pair(OP_ADD_REG, 0xFFFFFFFFu, 1u,        1, true,  0u, "ADD_REG carry-out");
    run_pair(OP_ADD_REG, 5u, 5u,                 1, true,  0u, "ADD_REG no-carry");
    run_pair(OP_ADD_REG, 0u, 0u,                 1, true,  0u, "ADD_REG zero");
    /* T1 SUB_REG always sets NZCV; ARM_C = no-borrow */
    run_pair(OP_SUB_REG, 0u, 1u,                 1, true,  0u, "SUB_REG borrow N");
    run_pair(OP_SUB_REG, 5u, 5u,                 1, true,  0u, "SUB_REG zero C-set");
    run_pair(OP_SUB_REG, 10u, 5u,                1, true,  0u, "SUB_REG no-borrow");
    run_pair(OP_SUB_REG, 0x80000000u, 1u,        1, true,  0u, "SUB_REG V signed");
    /* AND/ORR/EOR: NZ-only, C and V preserved */
    run_pair(OP_AND_REG, 0xFF00FF00u, 0x00FF00FFu, 1, true, 0u, "AND_REG zero");
    run_pair(OP_AND_REG, 0xFF00FF00u, 0xF000F000u, 1, true, 0u, "AND_REG nonzero");
    run_pair(OP_ORR_REG, 0u, 0u,                 1, true,  0u, "ORR_REG zero");
    run_pair(OP_ORR_REG, 0x80000000u, 0u,        1, true,  0u, "ORR_REG N");
    run_pair(OP_EOR_REG, 0xAAAAAAAAu, 0xAAAAAAAAu, 1, true, 0u, "EOR_REG zero");
    /* T1 ADD_IMM3/IMM8: always set NZCV */
    run_pair(OP_ADD_IMM3, 0x7FFFFFFFu, 0u,       1, false, 1u, "ADD_IMM3 V");
    run_pair(OP_SUB_IMM3, 5u, 0u,                1, false, 5u, "SUB_IMM3 zero");
    run_pair(OP_SUB_IMM8, 10u, 0u,               1, false, 10u, "SUB_IMM8 zero");
    /* CMP family: NZCV, discard result */
    run_pair(OP_CMP_IMM, 5u, 0u,                 0, false, 5u, "CMP_IMM zero");
    run_pair(OP_CMP_IMM, 5u, 0u,                 0, false, 7u, "CMP_IMM negative");
    run_pair(OP_CMP_REG, 5u, 5u,                 0, true,  0u, "CMP_REG zero");
    run_pair(OP_CMP_REG, 5u, 10u,                0, true,  0u, "CMP_REG borrow");
    run_pair(OP_CMP_REG, 0x80000000u, 1u,        0, true,  0u, "CMP_REG V");
    run_pair(OP_CMN_REG, 0x7FFFFFFFu, 1u,        0, true,  0u, "CMN_REG V");
    run_pair(OP_CMN_REG, 0xFFFFFFFFu, 1u,        0, true,  0u, "CMN_REG carry");
    run_pair(OP_TST_REG, 0xF0F0F0F0u, 0x0F0F0F0Fu, 0, true, 0u, "TST_REG zero");
    run_pair(OP_TST_REG, 0x80000000u, 0x80000000u, 0, true, 0u, "TST_REG N");
    /* T32 CMP/CMN */
    run_pair(OP_T32_CMP_IMM, 0xFFFFFFFFu, 0u,    0, false, 0xFFFFFFFFu, "T32 CMP_IMM zero");
    run_pair(OP_T32_CMP_REG, 100u, 50u,          0, true,  0u, "T32 CMP_REG no-borrow");
    /* T32 AND_IMM with set_flags */
    run_pair(OP_T32_AND_IMM, 0xFF00FF00u, 0u,    1, false, 0x00FF00FFu, "T32 AND_IMM zero");
    run_pair(OP_T32_AND_IMM, 0x80000000u, 0u,    1, false, 0x80000000u, "T32 AND_IMM N");
    /* T32 ADD_IMM with set_flags=0: must NOT update apsr */
    run_pair(OP_T32_ADD_IMM, 5u, 0u,             0, false, 3u, "T32 ADD_IMM no-flag");
    /* CMP_REG_T2 (hi-register form) */
    run_pair(OP_CMP_REG_T2, 42u, 42u,            0, true,  0u, "CMP_REG_T2 zero");
    run_pair(OP_CMP_REG_T2, 0x80000000u, 1u,     0, true,  0u, "CMP_REG_T2 V");

    TEST_REPORT();
}
