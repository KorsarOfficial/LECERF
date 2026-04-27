#include "test_harness.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/jit.h"
#include "core/codegen.h"
#include <string.h>

/* Large static: jit_t ~2MB; Windows 1MB stack would overflow if on stack. */
static jit_t s_jit;

TEST(abi_single_insn) {
    bus_t bus; bus_init(&bus);
    bus_add_flat(&bus, "flash", 0x00000000u, 4096u, false);
    bus_add_flat(&bus, "sram",  0x20000000u, 4096u, true);

    cpu_t cpu; memset(&cpu, 0, sizeof cpu);
    cpu_reset(&cpu, CORE_M4);
    cpu.r[REG_PC] = 0u;

    memset(&s_jit, 0, sizeof s_jit);
    jit_init(&s_jit);

    /* MOV.W r0, #0xDEADBEEF via OP_T32_MOVW */
    insn_t ins; memset(&ins, 0, sizeof ins);
    ins.op   = OP_T32_MOVW;
    ins.rd   = 0u;
    ins.imm  = 0xDEADBEEFu;
    ins.pc   = 0u;
    ins.size = 4u;

    cg_thunk_t fn = codegen_emit(&s_jit.cg, &bus, &ins, 1u);
    ASSERT_TRUE(fn != NULL);

    /* WIN64 call: rcx=&cpu, rdx=&bus; prologue saves to r15/r14 */
    bool ok = fn(&cpu, &bus);
    ASSERT_TRUE(ok);
    ASSERT_EQ_U32(cpu.r[0], 0xDEADBEEFu);
    ASSERT_EQ_U32(cpu.r[REG_PC], ins.pc + ins.size);
}

TEST(abi_jit_flush) {
    s_jit.n_blocks      = 7u;
    s_jit.lookup_n      = 13;
    s_jit.cg.used       = 12345u;
    s_jit.counters[42]  = 99u;
    s_jit.lookup_idx[10] = 5;
    s_jit.lookup_pc[10]  = 0xCAFEBABEu;

    jit_flush(&s_jit);
    ASSERT_EQ_U32(s_jit.n_blocks, 0u);
    ASSERT_TRUE(s_jit.lookup_n == 0);
    ASSERT_EQ_U32(s_jit.cg.used, 0u);
    ASSERT_EQ_U32(s_jit.counters[42], 0u);
    ASSERT_TRUE(s_jit.lookup_idx[10] == -1);
    ASSERT_EQ_U32(s_jit.lookup_pc[10], 0u);
}

TEST(abi_two_insn_block) {
    bus_t bus; bus_init(&bus);
    bus_add_flat(&bus, "flash", 0x00000000u, 4096u, false);
    bus_add_flat(&bus, "sram",  0x20000000u, 4096u, true);

    cpu_t cpu; memset(&cpu, 0, sizeof cpu);
    cpu_reset(&cpu, CORE_M4);
    cpu.r[0] = 0u; cpu.r[1] = 0u; cpu.r[REG_PC] = 0u;

    /* After jit_flush cg.used=0; codegen writes from offset 0 again. */
    insn_t a; memset(&a, 0, sizeof a);
    a.op = OP_T32_MOVW; a.rd = 0u; a.imm = 0x11112222u; a.pc = 0u; a.size = 4u;

    insn_t b; memset(&b, 0, sizeof b);
    b.op = OP_T32_MOVW; b.rd = 1u; b.imm = 0x33334444u; b.pc = 4u; b.size = 4u;

    insn_t pair[2]; pair[0] = a; pair[1] = b;
    cg_thunk_t fn2 = codegen_emit(&s_jit.cg, &bus, pair, 2u);
    ASSERT_TRUE(fn2 != NULL);
    ASSERT_TRUE(fn2(&cpu, &bus));
    ASSERT_EQ_U32(cpu.r[0], 0x11112222u);
    ASSERT_EQ_U32(cpu.r[1], 0x33334444u);
    ASSERT_EQ_U32(cpu.r[REG_PC], 8u);
}

int main(void) {
    RUN(abi_single_insn);
    RUN(abi_jit_flush);
    RUN(abi_two_insn_block);
    TEST_REPORT();
}
