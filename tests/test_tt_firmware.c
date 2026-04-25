#include "test_harness.h"
#include "core/tt.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/nvic.h"
#include "core/jit.h"
#include "core/run.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "periph/mpu.h"
#include "periph/dwt.h"
#include "periph/stm32.h"
#include "periph/eth.h"
#include "periph/uart.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FLASH_BASE 0x00000000u
#define FLASH_SZ   (1u << 20)

extern cpu_t*  g_cpu_for_scb;
extern dwt_t*  g_dwt_for_run;
extern nvic_t* g_nvic_for_run;

/* jit_t ~2MB each: file scope to avoid stack overflow */
static jit_t s_g1, s_g3;
/* snap_blob_t ~263KB each: file scope */
static snap_blob_t s_ref, s_r2, s_r3;

static u8* load_bin(const char* path, u32* out_n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* p = (u8*)malloc((size_t)n);
    fread(p, 1, (size_t)n, f); fclose(f);
    *out_n = (u32)n;
    return p;
}

/* Exact attach sequence from tools/main.c */
static void setup(cpu_t* c, bus_t* bus, jit_t* g, tt_periph_t* p,
                  systick_t* st, nvic_t* nv, scb_t* scb, mpu_t* mpu,
                  dwt_t* dwt, stm32_t* s, eth_t* e, uart_t* u,
                  const u8* fw, u32 fw_n) {
    bus_init(bus);
    bus_add_flat(bus, "flash", FLASH_BASE, FLASH_SZ,  false);
    bus_add_flat(bus, "sram",  SRAM_BASE_ADDR, SRAM_SIZE, true);
    memset(u, 0, sizeof *u);
    uart_attach(bus, u);
    systick_attach(bus, st);
    scb_attach(bus, scb);
    g_cpu_for_scb = c;
    mpu_attach(bus, mpu);
    stm32_attach(bus, s);
    s->quiet = true;
    dwt_attach(bus, dwt);
    g_dwt_for_run = dwt;
    nvic_attach(bus, nv);
    g_nvic_for_run = nv;
    eth_attach(bus, e);
    bus_load_blob(bus, FLASH_BASE, fw, fw_n);
    cpu_reset(c, CORE_M4);
    c->msp = bus_r32(bus, 0x0u);
    c->r[REG_SP] = c->msp;
    c->r[REG_PC] = bus_r32(bus, 0x4u) & ~1u;
    p->st = st; p->nv = nv; p->scb = scb;
    p->mpu = mpu; p->dwt = dwt; p->stm32 = s;
    p->eth = e; p->uart = u;
    memset(g, 0, sizeof *g);
    tt_attach_jit(g);
}

static void run_with_tt(cpu_t* c, bus_t* bus, jit_t* g, tt_periph_t* p,
                        tt_t* tt, u64 tgt) {
    while (c->cycles < tgt) {
        u64 gap = tgt - c->cycles;
        u64 batch = gap < 5000ull ? gap : 5000ull;
        run_steps_full_g(c, bus, batch, p->st, p->scb, g);
        tt_on_cycle(tt, c, bus, p);
    }
}

TEST(tt_firmware_three_run) {
    u32 fw_n = 0u;
    /* try project-root-relative path first (build/tests/ -> ../../firmware/test_tt/test_tt.bin) */
    u8* fw = load_bin("../../firmware/test_tt/test_tt.bin", &fw_n);
    if (!fw) fw = load_bin("../firmware/test_tt/test_tt.bin", &fw_n);
    if (!fw) fw = load_bin("firmware/test_tt/test_tt.bin",   &fw_n);
#ifdef FIRMWARE_BIN_PATH
    if (!fw) fw = load_bin(FIRMWARE_BIN_PATH, &fw_n);
#endif
    if (!fw) {
        fprintf(stderr, "SKIP: test_tt.bin not found (arm-none-eabi-gcc may be missing)\n");
        /* graceful skip: report 0 failures */
        return;
    }
    ASSERT_TRUE(fw_n > 0u);

    /* === RUN 1: REF === */
    cpu_t c1; bus_t bus1;
    systick_t st1; nvic_t nv1; scb_t scb1; mpu_t mpu1;
    dwt_t dwt1; stm32_t s1; eth_t e1; uart_t u1;
    tt_periph_t p1;
    setup(&c1, &bus1, &s_g1, &p1, &st1, &nv1, &scb1, &mpu1,
          &dwt1, &s1, &e1, &u1, fw, fw_n);
    tt_t* tt1 = tt_create(5000u, 50u);
    ASSERT_TRUE(tt1 != NULL);
    run_with_tt(&c1, &bus1, &s_g1, &p1, tt1, 50000ull);
    ASSERT_TRUE(snap_save(&s_ref, &c1, &bus1, &p1));

    /* === RUN 2: rewind to 25000, forward to 50000, byte-equal to REF === */
    ASSERT_TRUE(tt_rewind(tt1, 25000ull, &c1, &bus1, &p1, &s_g1));
    run_with_tt(&c1, &bus1, &s_g1, &p1, tt1, 50000ull);
    ASSERT_TRUE(snap_save(&s_r2, &c1, &bus1, &p1));
    ASSERT_TRUE(memcmp(&s_ref, &s_r2, sizeof s_ref) == 0);
    tt_destroy(tt1);

    /* === RUN 3: fresh state, run to 50000, step_back 10000, forward to 50000 === */
    cpu_t c3; bus_t bus3;
    systick_t st3; nvic_t nv3; scb_t scb3; mpu_t mpu3;
    dwt_t dwt3; stm32_t s3; eth_t e3; uart_t u3;
    tt_periph_t p3;
    setup(&c3, &bus3, &s_g3, &p3, &st3, &nv3, &scb3, &mpu3,
          &dwt3, &s3, &e3, &u3, fw, fw_n);
    tt_t* tt3 = tt_create(5000u, 50u);
    ASSERT_TRUE(tt3 != NULL);
    run_with_tt(&c3, &bus3, &s_g3, &p3, tt3, 50000ull);
    ASSERT_TRUE(tt_step_back(tt3, 10000ull, &c3, &bus3, &p3, &s_g3));
    run_with_tt(&c3, &bus3, &s_g3, &p3, tt3, 50000ull);
    ASSERT_TRUE(snap_save(&s_r3, &c3, &bus3, &p3));
    ASSERT_TRUE(memcmp(&s_ref, &s_r3, sizeof s_ref) == 0);
    tt_destroy(tt3);

    /* tt_diff sanity: ref vs ref -> empty output */
    FILE* f = fopen("self_diff.txt", "w");
    ASSERT_TRUE(f != NULL);
    tt_diff(&s_ref, &s_ref, f);
    long pos = ftell(f);
    fclose(f);
    ASSERT_TRUE(pos == 0);
    remove("self_diff.txt");

    free(fw);
}

int main(void) {
    RUN(tt_firmware_three_run);
    TEST_REPORT();
}
