#include "test_harness.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/codegen.h"
#include "core/decoder.h"
#include "core/jit.h"
#include <string.h>
#include <stdio.h>

/* jit_t ~2MB: static to avoid stack overflow on Windows (1MB default). */
static jit_t s_jit;

/* Shared bus/cpu reset for each sub-test; 64KB SRAM at 0x20000000. */
static bus_t g_bus;
static cpu_t g_cpu;

static void setup(void) {
    bus_init(&g_bus);
    bus_add_flat(&g_bus, "flash", 0x00000000u, 4096u, false);
    bus_add_flat(&g_bus, "sram",  0x20000000u, 65536u, true);
    memset(&g_cpu, 0, sizeof g_cpu);
    cpu_reset(&g_cpu, CORE_M4);
    memset(&s_jit, 0, sizeof s_jit);
    jit_init(&s_jit);
}

/* Read u32 little-endian from SRAM buffer at absolute address. */
static u32 sram_rd(u32 addr) {
    region_t* sram = bus_find_flat(&g_bus, 0x20000000u);
    if (!sram || !sram->buf) return 0xDEADDEADu;
    u32 off = addr - sram->base;
    return (u32)sram->buf[off]
         | ((u32)sram->buf[off+1] << 8)
         | ((u32)sram->buf[off+2] << 16)
         | ((u32)sram->buf[off+3] << 24);
}

/* Write u32 little-endian to SRAM buffer at absolute address. */
static void sram_wr(u32 addr, u32 val) {
    region_t* sram = bus_find_flat(&g_bus, 0x20000000u);
    if (!sram || !sram->buf) return;
    u32 off = addr - sram->base;
    sram->buf[off]   = (u8)val;
    sram->buf[off+1] = (u8)(val >> 8);
    sram->buf[off+2] = (u8)(val >> 16);
    sram->buf[off+3] = (u8)(val >> 24);
}

/* ---- Test 1: PUSH{r4,r5,r6,r7,lr} ---- */
/* reg_list = 0x40F0; SP = 0x20002000; expect 5 words written descending. */
TEST(push_hi_regs) {
    setup();
    g_cpu.r[4]     = 0x44444444u;
    g_cpu.r[5]     = 0x55555555u;
    g_cpu.r[6]     = 0x66666666u;
    g_cpu.r[7]     = 0x77777777u;
    g_cpu.r[REG_LR]= 0xFFFFFFFDu;  /* EXC_RETURN value (not popped, just stored) */
    g_cpu.r[REG_SP]= 0x20002000u;

    insn_t push; memset(&push, 0, sizeof push);
    push.op       = OP_PUSH;
    push.reg_list = (1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<14); /* {r4-r7,lr} */
    push.pc       = 0u;
    push.size     = 2u;

    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &push, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));

    /* SP must drop by 5*4=20 */
    ASSERT_EQ_U32(g_cpu.r[REG_SP], 0x20002000u - 20u);

    /* ARM PUSH stores registers in ascending order from new SP.
       new_sp=0x20001FEC; slot[0]=r4, slot[1]=r5, slot[2]=r6, slot[3]=r7, slot[4]=lr */
    u32 base = g_cpu.r[REG_SP];
    ASSERT_EQ_U32(sram_rd(base + 0u),  0x44444444u);
    ASSERT_EQ_U32(sram_rd(base + 4u),  0x55555555u);
    ASSERT_EQ_U32(sram_rd(base + 8u),  0x66666666u);
    ASSERT_EQ_U32(sram_rd(base + 12u), 0x77777777u);
    ASSERT_EQ_U32(sram_rd(base + 16u), 0xFFFFFFFDu);
}

/* ---- Test 2: PUSH{r0,r1,r2,r3} ---- */
TEST(push_lo_regs) {
    setup();
    g_cpu.r[0]     = 0x0A0A0A0Au;
    g_cpu.r[1]     = 0x1B1B1B1Bu;
    g_cpu.r[2]     = 0x2C2C2C2Cu;
    g_cpu.r[3]     = 0x3D3D3D3Du;
    g_cpu.r[REG_SP]= 0x20001000u;

    insn_t push; memset(&push, 0, sizeof push);
    push.op       = OP_PUSH;
    push.reg_list = 0x000Fu;  /* {r0-r3} */
    push.pc       = 0u;
    push.size     = 2u;

    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &push, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));

    ASSERT_EQ_U32(g_cpu.r[REG_SP], 0x20001000u - 16u);
    u32 base = g_cpu.r[REG_SP];
    ASSERT_EQ_U32(sram_rd(base + 0u),  0x0A0A0A0Au);
    ASSERT_EQ_U32(sram_rd(base + 4u),  0x1B1B1B1Bu);
    ASSERT_EQ_U32(sram_rd(base + 8u),  0x2C2C2C2Cu);
    ASSERT_EQ_U32(sram_rd(base + 12u), 0x3D3D3D3Du);
}

/* ---- Test 3: POP{r4,r5,r6,r7} (no PC) ---- */
TEST(pop_hi_regs) {
    setup();
    /* Pre-load SRAM at SP=0x20001000 */
    u32 sp0 = 0x20001000u;
    sram_wr(sp0 + 0u,  0xAAAA1111u);
    sram_wr(sp0 + 4u,  0xBBBB2222u);
    sram_wr(sp0 + 8u,  0xCCCC3333u);
    sram_wr(sp0 + 12u, 0xDDDD4444u);
    g_cpu.r[REG_SP] = sp0;

    insn_t pop; memset(&pop, 0, sizeof pop);
    pop.op       = OP_POP;
    pop.reg_list = (1u<<4)|(1u<<5)|(1u<<6)|(1u<<7);  /* {r4-r7} */
    pop.pc       = 0u;
    pop.size     = 2u;

    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &pop, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));

    ASSERT_EQ_U32(g_cpu.r[REG_SP], sp0 + 16u);
    ASSERT_EQ_U32(g_cpu.r[4], 0xAAAA1111u);
    ASSERT_EQ_U32(g_cpu.r[5], 0xBBBB2222u);
    ASSERT_EQ_U32(g_cpu.r[6], 0xCCCC3333u);
    ASSERT_EQ_U32(g_cpu.r[7], 0xDDDD4444u);
}

/* ---- Test 4: POP{r0,r1,r2,r3} ---- */
TEST(pop_lo_regs) {
    setup();
    u32 sp0 = 0x20002000u;
    sram_wr(sp0 + 0u,  0x00112233u);
    sram_wr(sp0 + 4u,  0x44556677u);
    sram_wr(sp0 + 8u,  0x8899AABBu);
    sram_wr(sp0 + 12u, 0xCCDDEEFFu);
    g_cpu.r[REG_SP] = sp0;

    insn_t pop; memset(&pop, 0, sizeof pop);
    pop.op       = OP_POP;
    pop.reg_list = 0x000Fu;  /* {r0-r3} */
    pop.pc       = 0u;
    pop.size     = 2u;

    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &pop, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));

    ASSERT_EQ_U32(g_cpu.r[REG_SP], sp0 + 16u);
    ASSERT_EQ_U32(g_cpu.r[0], 0x00112233u);
    ASSERT_EQ_U32(g_cpu.r[1], 0x44556677u);
    ASSERT_EQ_U32(g_cpu.r[2], 0x8899AABBu);
    ASSERT_EQ_U32(g_cpu.r[3], 0xCCDDEEFFu);
}

/* ---- Test 5: T32 LDM.IA r1!,{r2,r5} ---- */
TEST(t32_ldm_ia) {
    setup();
    u32 base = 0x20003000u;
    sram_wr(base + 0u, 0x11223344u);
    sram_wr(base + 4u, 0x55667788u);
    g_cpu.r[1] = base;  /* base register */
    g_cpu.r[2] = 0u;
    g_cpu.r[5] = 0u;

    insn_t ldm; memset(&ldm, 0, sizeof ldm);
    ldm.op       = OP_T32_LDM;
    ldm.rn       = 1u;
    ldm.reg_list = (1u<<2)|(1u<<5);  /* {r2,r5} */
    ldm.add      = true;    /* IA */
    ldm.writeback= true;
    ldm.pc       = 0u;
    ldm.size     = 4u;

    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &ldm, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));

    ASSERT_EQ_U32(g_cpu.r[2], 0x11223344u);
    ASSERT_EQ_U32(g_cpu.r[5], 0x55667788u);
    /* writeback: r1 = base + 2*4 */
    ASSERT_EQ_U32(g_cpu.r[1], base + 8u);
}

/* ---- Test 6: T32 STM.IA r0!,{r2,r5} ---- */
TEST(t32_stm_ia) {
    setup();
    u32 base = 0x20004000u;
    g_cpu.r[0] = base;
    g_cpu.r[2] = 0xCAFEBABEu;
    g_cpu.r[5] = 0xDEADBEEFu;

    insn_t stm; memset(&stm, 0, sizeof stm);
    stm.op       = OP_T32_STM;
    stm.rn       = 0u;
    stm.reg_list = (1u<<2)|(1u<<5);  /* {r2,r5} */
    stm.add      = true;    /* IA */
    stm.writeback= true;
    stm.pc       = 0u;
    stm.size     = 4u;

    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &stm, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));

    ASSERT_EQ_U32(sram_rd(base + 0u), 0xCAFEBABEu);
    ASSERT_EQ_U32(sram_rd(base + 4u), 0xDEADBEEFu);
    /* writeback: r0 = base + 2*4 */
    ASSERT_EQ_U32(g_cpu.r[0], base + 8u);
}

int main(void) {
    RUN(push_hi_regs);
    RUN(push_lo_regs);
    RUN(pop_hi_regs);
    RUN(pop_lo_regs);
    RUN(t32_ldm_ia);
    RUN(t32_stm_ia);
    TEST_REPORT();
}
