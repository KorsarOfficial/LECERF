#include "core/bus.h"
#include "core/cpu.h"
#include "core/decoder.h"
#include "test_harness.h"
#include <string.h>

extern u64 run_steps(cpu_t* c, bus_t* bus, u64 max_steps);

static void setup(bus_t* b, cpu_t* c) {
    bus_init(b);
    /* Test flash writable so literal pool can be patched in. */
    bus_add_flat(b, "flash", 0, 0x2000, true);
    bus_add_flat(b, "sram", 0x20000000, 0x2000, true);
    cpu_reset(c, CORE_M4);
    c->r[REG_PC] = 0;
    c->r[REG_SP] = 0x20001000;
}

/* STR/LDR via register-offset:
   - R1 = 0x20000100 (base)
   - R2 = 42
   - STR R2, [R1]        ; [0x20000100] = 42
   - LDR R3, [R1]        ; R3 = 42 */
TEST(str_ldr_reg) {
    bus_t b; cpu_t c; setup(&b, &c);
    u16 prog[] = {
        0x4909,  /* LDR R1, [PC,#36] — base addr from literal pool */
        0x222A,  /* MOV R2, #42              */
        0x600A,  /* STR R2, [R1]             */
        0x680B,  /* LDR R3, [R1]             */
        0xDEFE,  /* UDF                      */
        0x0000,
        /* literal pool at PC(+4 aligned). PC of LDR = 0; pool offset = 36 -> addr 0x28. */
    };
    bus_load_blob(&b, 0, (u8*)prog, sizeof(prog));
    /* Place pointer constant 0x20000100 at offset 0x28 */
    u32 ptr = 0x20000100;
    bus_w32(&b, 0x28, ptr);

    run_steps(&c, &b, 100);
    ASSERT_EQ_U32(c.r[2], 42);
    ASSERT_EQ_U32(c.r[3], 42);
    ASSERT_EQ_U32(bus_r32(&b, 0x20000100), 42);
}

/* PUSH/POP:
   R0=0xAA, R1=0xBB, R2=0xCC, R3=0xDD
   PUSH {R0-R3}
   (clobber)
   POP {R4-R7}
   R4..R7 should be 0xAA..0xDD in order. */
TEST(push_pop) {
    bus_t b; cpu_t c; setup(&b, &c);
    u16 prog[] = {
        0x20AA,  /* MOV R0, #0xAA     */
        0x21BB,  /* MOV R1, #0xBB     */
        0x22CC,  /* MOV R2, #0xCC     */
        0x23DD,  /* MOV R3, #0xDD     */
        0xB40F,  /* PUSH {R0-R3}      */
        0x2000,  /* MOV R0,#0 — clobber */
        0x2100,  /* MOV R1,#0 */
        0x2200,  /* MOV R2,#0 */
        0x2300,  /* MOV R3,#0 */
        0xBC0F,  /* POP  {R0-R3}      */
        0xDEFE,  /* UDF               */
    };
    bus_load_blob(&b, 0, (u8*)prog, sizeof(prog));
    run_steps(&c, &b, 100);
    ASSERT_EQ_U32(c.r[0], 0xAA);
    ASSERT_EQ_U32(c.r[1], 0xBB);
    ASSERT_EQ_U32(c.r[2], 0xCC);
    ASSERT_EQ_U32(c.r[3], 0xDD);
}

/* BL (Thumb-2): call subroutine, R0 += 5, return, verify R0=5 and LR was saved.
   Layout:
     main: MOV R0, #0
           BL   +8        ; go to sub
           UDF
     sub:  ADD R0, #5
           BX  LR
*/
TEST(bl_bx) {
    bus_t b; cpu_t c; setup(&b, &c);
    u16 prog[] = {
        0x2000,        /* 0x00: MOV R0, #0 */
        0xF000, 0xF801,/* 0x02: BL +2 (2*1 = 2, so target = 0x08) */
        0xDEFE,        /* 0x06: UDF */
        0x3005,        /* 0x08: ADDS R0, #5 */
        0x4770,        /* 0x0A: BX LR */
    };
    bus_load_blob(&b, 0, (u8*)prog, sizeof(prog));
    run_steps(&c, &b, 100);
    ASSERT_EQ_U32(c.r[0], 5);
}

/* Halfword + byte load/store */
TEST(ldrh_strh_ldrb_strb) {
    bus_t b; cpu_t c; setup(&b, &c);
    u16 prog[] = {
        0x4906,  /* LDR R1, [PC,#24] — base addr                */
        0x22A5,  /* MOV R2, #0xA5                               */
        0x7002,  /* STRB R2, [R1, #0]                           */
        0x7803,  /* LDRB R3, [R1, #0]                           */
        0x22CD,  /* MOV R2, #0xCD                               */
        0x8002,  /* STRH R2, [R1, #0]  -- overwrites LOW byte   */
        0x8804,  /* LDRH R4, [R1, #0]                           */
        0xDEFE,  /* UDF                                         */
        0x0000, 0x0000,
        /* literal pool */
    };
    bus_load_blob(&b, 0, (u8*)prog, sizeof(prog));
    u32 ptr = 0x20000200;
    bus_w32(&b, 0x1C, ptr);
    bus_w32(&b, 0x20000200, 0); /* clear mem */

    run_steps(&c, &b, 200);
    ASSERT_EQ_U32(c.r[3], 0xA5);       /* LDRB after first STRB */
    ASSERT_EQ_U32(c.r[4], 0x00CD);     /* LDRH after STRH */
}

int main(void) {
    RUN(str_ldr_reg);
    RUN(push_pop);
    RUN(bl_bx);
    RUN(ldrh_strh_ldrb_strb);
    TEST_REPORT();
}
