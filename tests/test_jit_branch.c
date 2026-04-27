#include "test_harness.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/codegen.h"
#include "core/decoder.h"
#include "core/jit.h"
#include <string.h>
#include <stdio.h>

static jit_t s_jit;

/* Replicate cond_pass from executor.c for ground-truth comparison. */
static bool cond_truth(u32 apsr, u8 cond) {
    bool n = (apsr >> 31) & 1u;
    bool z = (apsr >> 30) & 1u;
    bool c = (apsr >> 29) & 1u;
    bool v = (apsr >> 28) & 1u;
    switch (cond) {
        case 0x0: return z;
        case 0x1: return !z;
        case 0x2: return c;
        case 0x3: return !c;
        case 0x4: return n;
        case 0x5: return !n;
        case 0x6: return v;
        case 0x7: return !v;
        case 0x8: return c && !z;
        case 0x9: return !c || z;
        case 0xA: return n == v;
        case 0xB: return n != v;
        case 0xC: return !z && (n == v);
        case 0xD: return z || (n != v);
        default:  return true;
    }
}

/* Build [CMP r1,r2; B<cond> +imm] thunk; run; verify PC matches cond_truth. */
static void run_branch(u32 a, u32 b, u8 cond, u32 imm, const char* tag) {
    bus_t bus; bus_init(&bus);
    bus_add_flat(&bus, "flash", 0x00000000u, 4096u, false);
    bus_add_flat(&bus, "sram",  0x20000000u, 4096u, true);

    cpu_t c; memset(&c, 0, sizeof c);
    cpu_reset(&c, CORE_M4);
    c.r[1] = a; c.r[2] = b; c.r[REG_PC] = 0u; c.apsr = 0u;

    /* CMP r1, r2 at pc=0, size=2 */
    insn_t cmp; memset(&cmp, 0, sizeof cmp);
    cmp.op = OP_CMP_REG; cmp.rn = 1u; cmp.rm = 2u;
    cmp.pc = 0u; cmp.size = 2u;

    /* B<cond> +imm at pc=2, size=2 */
    insn_t bi; memset(&bi, 0, sizeof bi);
    bi.op = OP_B_COND; bi.cond = cond;
    bi.imm = (u32)(i32)(i8)((int)imm & 0xFF); /* signed byte offset, already pre-shifted */
    /* imm is passed directly as the signed byte offset (decoder pre-shifts) */
    bi.imm = imm;
    bi.pc = 2u; bi.size = 2u;

    insn_t pair[2]; pair[0] = cmp; pair[1] = bi;

    memset(&s_jit, 0, sizeof s_jit);
    jit_init(&s_jit);
    cg_thunk_t fn = codegen_emit(&s_jit.cg, pair, 2u);

    _g_tests++;
    if (!fn) { fprintf(stderr, "FAIL %s: codegen_emit returned NULL\n", tag); _g_fail++; return; }

    bool ok = fn(&c, &bus);
    _g_tests++;
    if (!ok) { fprintf(stderr, "FAIL %s: thunk returned false\n", tag); _g_fail++; }

    /* CMP sets APSR; compute expected PC from APSR after execution */
    u32 exp_pc;
    if (cond_truth(c.apsr, cond))
        exp_pc = 2u + 4u + imm;   /* taken: B.cond at pc=2, target=pc+4+imm */
    else
        exp_pc = 2u + 2u;         /* fallthrough: pc + size */

    _g_tests++;
    if (c.r[REG_PC] != exp_pc) {
        fprintf(stderr, "FAIL %s: cond=%x a=%08x b=%08x apsr=%08x: pc=%08x exp=%08x\n",
                tag, (unsigned)cond, a, b, c.apsr, c.r[REG_PC], exp_pc);
        _g_fail++;
    }
}

TEST(branch_eq) {
    run_branch(5u, 5u,  0x0, 0x10u, "EQ taken");
    run_branch(5u, 6u,  0x0, 0x10u, "EQ fall");
}

TEST(branch_ne) {
    run_branch(5u, 6u,  0x1, 0x10u, "NE taken");
    run_branch(5u, 5u,  0x1, 0x10u, "NE fall");
}

TEST(branch_cs) {
    run_branch(10u, 5u, 0x2, 0x10u, "CS taken");
    run_branch(5u, 10u, 0x2, 0x10u, "CS fall");
}

TEST(branch_cc) {
    run_branch(5u, 10u, 0x3, 0x10u, "CC taken");
    run_branch(10u, 5u, 0x3, 0x10u, "CC fall");
}

TEST(branch_mi) {
    run_branch(0u, 1u,  0x4, 0x10u, "MI taken");   /* 0-1 -> negative, N=1 */
    run_branch(5u, 0u,  0x4, 0x10u, "MI fall");
}

TEST(branch_pl) {
    run_branch(5u, 0u,  0x5, 0x10u, "PL taken");
    run_branch(0u, 1u,  0x5, 0x10u, "PL fall");
}

TEST(branch_vs) {
    /* signed overflow: 0x80000000 - 1 = 0x7FFFFFFF, V=1 */
    run_branch(0x80000000u, 1u, 0x6, 0x10u, "VS taken");
    run_branch(5u, 1u,          0x6, 0x10u, "VS fall");
}

TEST(branch_vc) {
    run_branch(5u, 1u,          0x7, 0x10u, "VC taken");
    run_branch(0x80000000u, 1u, 0x7, 0x10u, "VC fall");
}

TEST(branch_hi) {
    run_branch(10u, 5u, 0x8, 0x10u, "HI taken");
    run_branch(5u,  5u, 0x8, 0x10u, "HI fall eq");
    run_branch(5u, 10u, 0x8, 0x10u, "HI fall lo");
}

TEST(branch_ls) {
    run_branch(5u,  5u, 0x9, 0x10u, "LS taken eq");
    run_branch(5u, 10u, 0x9, 0x10u, "LS taken lo");
    run_branch(10u, 5u, 0x9, 0x10u, "LS fall");
}

TEST(branch_ge) {
    run_branch(10u, 5u, 0xA, 0x10u, "GE taken");
    run_branch(5u,  5u, 0xA, 0x10u, "GE taken eq");
    run_branch(0u,  1u, 0xA, 0x10u, "GE fall");
}

TEST(branch_lt) {
    run_branch(0u,  1u, 0xB, 0x10u, "LT taken");
    run_branch(10u, 5u, 0xB, 0x10u, "LT fall");
}

TEST(branch_gt) {
    run_branch(10u, 5u, 0xC, 0x10u, "GT taken");
    run_branch(5u,  5u, 0xC, 0x10u, "GT fall eq");
    run_branch(0u,  1u, 0xC, 0x10u, "GT fall lt");
}

TEST(branch_le) {
    run_branch(5u,  5u, 0xD, 0x10u, "LE taken eq");
    run_branch(0u,  1u, 0xD, 0x10u, "LE taken lt");
    run_branch(10u, 5u, 0xD, 0x10u, "LE fall");
}

TEST(branch_uncond) {
    bus_t bus; bus_init(&bus);
    bus_add_flat(&bus, "flash", 0x00000000u, 4096u, false);
    bus_add_flat(&bus, "sram",  0x20000000u, 4096u, true);
    cpu_t c; memset(&c, 0, sizeof c); cpu_reset(&c, CORE_M4); c.r[REG_PC] = 0u;
    insn_t b; memset(&b, 0, sizeof b);
    b.op = OP_B_UNCOND; b.imm = 0x20u; b.pc = 0u; b.size = 2u;
    memset(&s_jit, 0, sizeof s_jit); jit_init(&s_jit);
    cg_thunk_t fn = codegen_emit(&s_jit.cg, &b, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&c, &bus));
    ASSERT_EQ_U32(c.r[REG_PC], 0u + 4u + 0x20u);
}

TEST(branch_bl) {
    bus_t bus; bus_init(&bus);
    bus_add_flat(&bus, "flash", 0x00000000u, 4096u, false);
    bus_add_flat(&bus, "sram",  0x20000000u, 4096u, true);
    cpu_t c; memset(&c, 0, sizeof c); cpu_reset(&c, CORE_M4);
    c.r[REG_PC] = 0x100u; c.r[REG_LR] = 0u;
    insn_t bl; memset(&bl, 0, sizeof bl);
    bl.op = OP_T32_BL; bl.imm = 0x40u; bl.pc = 0x100u; bl.size = 4u;
    memset(&s_jit, 0, sizeof s_jit); jit_init(&s_jit);
    cg_thunk_t fn = codegen_emit(&s_jit.cg, &bl, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&c, &bus));
    ASSERT_EQ_U32(c.r[REG_LR], (0x100u + 4u) | 1u);
    ASSERT_EQ_U32(c.r[REG_PC], (0x100u + 4u + 0x40u) & ~1u);
}

int main(void) {
    RUN(branch_eq);
    RUN(branch_ne);
    RUN(branch_cs);
    RUN(branch_cc);
    RUN(branch_mi);
    RUN(branch_pl);
    RUN(branch_vs);
    RUN(branch_vc);
    RUN(branch_hi);
    RUN(branch_ls);
    RUN(branch_ge);
    RUN(branch_lt);
    RUN(branch_gt);
    RUN(branch_le);
    RUN(branch_uncond);
    RUN(branch_bl);
    TEST_REPORT();
}
