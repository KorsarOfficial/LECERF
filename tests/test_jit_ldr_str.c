#include "test_harness.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/codegen.h"
#include "core/decoder.h"
#include "core/jit.h"
#include <string.h>

/* jit_t ~2MB: must not be on stack (Windows 1MB default stack). */
static jit_t s_jit;

/* Shared bus/cpu for all sub-tests; SRAM 64K at 0x20000000. */
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

/* Test 1: STR r0 -> 0x20001000; LDR r1 <- 0x20001000; expect r1 == r0. */
TEST(ldr_str_roundtrip) {
    setup();
    g_cpu.r[0] = 0x12345678u;
    g_cpu.r[1] = 0x0u;
    g_cpu.r[2] = 0x20001000u;   /* base */

    insn_t str; memset(&str, 0, sizeof str);
    str.op = OP_STR_IMM; str.rd = 0u; str.rn = 2u; str.imm = 0u;
    str.pc = 0u; str.size = 2u;
    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &str, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));

    /* Verify byte image in SRAM at offset 0x1000 (= 0x20001000 - 0x20000000) */
    region_t* sram = bus_find_flat(&g_bus, 0x20000000u);
    ASSERT_TRUE(sram != NULL);
    u32 stored = (u32)sram->buf[0x1000]
               | ((u32)sram->buf[0x1001] << 8)
               | ((u32)sram->buf[0x1002] << 16)
               | ((u32)sram->buf[0x1003] << 24);
    ASSERT_EQ_U32(stored, 0x12345678u);

    insn_t ldr; memset(&ldr, 0, sizeof ldr);
    ldr.op = OP_LDR_IMM; ldr.rd = 1u; ldr.rn = 2u; ldr.imm = 0u;
    ldr.pc = 2u; ldr.size = 2u;
    cg_thunk_t fn2 = codegen_emit(&s_jit.cg, &g_bus, &ldr, 1u);
    ASSERT_TRUE(fn2 != NULL);
    ASSERT_TRUE(fn2(&g_cpu, &g_bus));
    ASSERT_EQ_U32(g_cpu.r[1], 0x12345678u);
}

/* Test 2: LDRB zero-extends (0xFF -> 0x000000FF, not 0xFFFFFFFF). */
TEST(ldrb_zero_extend) {
    setup();
    region_t* sram = bus_find_flat(&g_bus, 0x20000000u);
    ASSERT_TRUE(sram != NULL);
    sram->buf[0x2000] = 0xFFu;
    g_cpu.r[2] = 0x20002000u;
    g_cpu.r[3] = 0xAAAAAAAAu;

    insn_t ldrb; memset(&ldrb, 0, sizeof ldrb);
    ldrb.op = OP_LDRB_IMM; ldrb.rd = 3u; ldrb.rn = 2u; ldrb.imm = 0u;
    ldrb.pc = 0u; ldrb.size = 2u;
    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &ldrb, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));
    ASSERT_EQ_U32(g_cpu.r[3], 0x000000FFu);
}

/* Test 3: LDRH zero-extends (little-endian 0xCAFE -> 0x0000CAFE). */
TEST(ldrh_zero_extend) {
    setup();
    region_t* sram = bus_find_flat(&g_bus, 0x20000000u);
    ASSERT_TRUE(sram != NULL);
    sram->buf[0x3000] = 0xFEu;   /* low byte */
    sram->buf[0x3001] = 0xCAu;   /* high byte -> 0xCAFE */
    g_cpu.r[2] = 0x20003000u;
    g_cpu.r[4] = 0xBBBBBBBBu;

    insn_t ldrh; memset(&ldrh, 0, sizeof ldrh);
    ldrh.op = OP_LDRH_IMM; ldrh.rd = 4u; ldrh.rn = 2u; ldrh.imm = 0u;
    ldrh.pc = 0u; ldrh.size = 2u;
    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &ldrh, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));
    ASSERT_EQ_U32(g_cpu.r[4], 0x0000CAFEu);
}

/* Test 4: T32 wide LDR_IMM uses same emit_load path. */
TEST(t32_ldr_imm) {
    setup();
    region_t* sram = bus_find_flat(&g_bus, 0x20000000u);
    ASSERT_TRUE(sram != NULL);
    /* Pre-load value via STR thunk */
    g_cpu.r[0] = 0xDEADBEEFu;
    g_cpu.r[2] = 0x20001000u;
    insn_t str; memset(&str, 0, sizeof str);
    str.op = OP_STR_IMM; str.rd = 0u; str.rn = 2u; str.imm = 0u;
    str.pc = 0u; str.size = 2u;
    cg_thunk_t sf = codegen_emit(&s_jit.cg, &g_bus, &str, 1u);
    ASSERT_TRUE(sf != NULL);
    ASSERT_TRUE(sf(&g_cpu, &g_bus));

    g_cpu.r[5] = 0u;
    insn_t t32; memset(&t32, 0, sizeof t32);
    t32.op = OP_T32_LDR_IMM; t32.rd = 5u; t32.rn = 2u; t32.imm = 0u;
    t32.pc = 2u; t32.size = 4u;
    t32.add = true; t32.index = true; t32.writeback = false; /* T3 simple offset */
    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &t32, 1u);
    ASSERT_TRUE(fn != NULL);
    ASSERT_TRUE(fn(&g_cpu, &g_bus));
    ASSERT_EQ_U32(g_cpu.r[5], 0xDEADBEEFu);
}

/* Test 5: bus fault path — LDR from unmapped addr -> halted=1, thunk returns false. */
TEST(ldr_fault_path) {
    setup();
    g_cpu.r[2]    = 0xFFFFFFF0u;   /* unmapped */
    g_cpu.halted  = false;
    g_cpu.r[6]    = 0xCCCCCCCCu;

    insn_t bad; memset(&bad, 0, sizeof bad);
    bad.op = OP_LDR_IMM; bad.rd = 6u; bad.rn = 2u; bad.imm = 0u;
    bad.pc = 0u; bad.size = 2u;
    cg_thunk_t fn = codegen_emit(&s_jit.cg, &g_bus, &bad, 1u);
    ASSERT_TRUE(fn != NULL);
    bool ok = fn(&g_cpu, &g_bus);
    ASSERT_TRUE(!ok);             /* thunk must return false */
    ASSERT_TRUE(g_cpu.halted);    /* halted flag must be set */
    ASSERT_EQ_U32(g_cpu.r[6], 0xCCCCCCCCu);  /* r[6] must not be clobbered */
}

int main(void) {
    RUN(ldr_str_roundtrip);
    RUN(ldrb_zero_extend);
    RUN(ldrh_zero_extend);
    RUN(t32_ldr_imm);
    RUN(ldr_fault_path);
    TEST_REPORT();
}
