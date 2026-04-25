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

/* Minimal Thumb-2 blob: vector table (SP=0x20040000, entry=0x8|1),
   then NOP x3, then B . (infinite loop at 0xE).
   No real IRQ handler — IRQ inject just marks pending in nvic without firing
   (IRQ not enabled in NVIC enable regs). UART RX byte goes into rx_q.
   Both runs see identical event injections at same cycles -> byte-equal state. */
static const u8 k_blob[] = {
    0x00, 0x00, 0x04, 0x20,  /* [0x00] initial SP = 0x20040000 */
    0x09, 0x00, 0x00, 0x00,  /* [0x04] reset handler = 0x8|1 (Thumb) */
    0x00, 0xBF,              /* [0x08] NOP */
    0x00, 0xBF,              /* [0x0A] NOP */
    0x00, 0xBF,              /* [0x0C] NOP */
    0xFE, 0xE7,              /* [0x0E] B . (loop) */
};

extern cpu_t*  g_cpu_for_scb;
extern dwt_t*  g_dwt_for_run;
extern nvic_t* g_nvic_for_run;

/* Large statics: jit_t ~2MB each, snap_blob_t ~263KB. */
static jit_t s_g1, s_g2;
static snap_blob_t s_snap;

static void setup_one(cpu_t* c, bus_t* bus, tt_periph_t* p,
                      systick_t* st, nvic_t* nv, scb_t* scb,
                      mpu_t* mpu, dwt_t* dwt, stm32_t* stm32,
                      eth_t* eth, uart_t* u) {
    bus_init(bus);
    bus_add_flat(bus, "flash", 0x00000000u, 1024u * 1024u, false);
    bus_add_flat(bus, "sram",  SRAM_BASE_ADDR, SRAM_SIZE, true);
    memset(u, 0, sizeof *u);
    uart_attach(bus, u);
    systick_attach(bus, st);
    scb_attach(bus, scb);
    g_cpu_for_scb = c;
    mpu_attach(bus, mpu);
    stm32_attach(bus, stm32);
    stm32->quiet = true;
    dwt_attach(bus, dwt);
    g_dwt_for_run = dwt;
    nvic_attach(bus, nv);
    g_nvic_for_run = nv;
    eth_attach(bus, eth);
    bus_load_blob(bus, 0x00000000u, k_blob, (u32)sizeof k_blob);
    cpu_reset(c, CORE_M4);
    c->msp = bus_r32(bus, 0x0u);
    c->r[REG_SP] = c->msp;
    c->r[REG_PC] = bus_r32(bus, 0x4u) & ~1u;
    p->st = st; p->nv = nv; p->scb = scb;
    p->mpu = mpu; p->dwt = dwt; p->stm32 = stm32;
    p->eth = eth; p->uart = u;
}

/* TT-05: two independent tt_replay calls with same (snap, log, target)
   produce byte-equal cpu_t and SRAM. */
TEST(tt_replay_byte_equal) {
    cpu_t c1, c2;
    bus_t bus1, bus2;
    systick_t st1, st2;
    nvic_t nv1, nv2;
    scb_t scb1, scb2;
    mpu_t mpu1, mpu2;
    dwt_t dwt1, dwt2;
    stm32_t s1, s2;
    eth_t e1, e2;
    uart_t u1, u2;
    tt_periph_t p1, p2;

    memset(&s_g1, 0, sizeof s_g1);
    memset(&s_g2, 0, sizeof s_g2);

    setup_one(&c1, &bus1, &p1, &st1, &nv1, &scb1, &mpu1, &dwt1, &s1, &e1, &u1);
    setup_one(&c2, &bus2, &p2, &st2, &nv2, &scb2, &mpu2, &dwt2, &s2, &e2, &u2);

    /* Snapshot at cycle 0 (initial state after cpu_reset). */
    ASSERT_TRUE(snap_save(&s_snap, &c1, &bus1, &p1));

    /* Synthetic event log: IRQ at 5000, UART RX at 7000. */
    ev_log_t lg;
    ev_log_init(&lg, 16u);
    ev_log_append(&lg, 5000u, EVENT_IRQ_INJECT, 16u);
    ev_log_append(&lg, 7000u, EVENT_UART_RX, (u32)'X');

    /* Replay 1 -> c1/bus1. */
    ASSERT_TRUE(tt_replay(&s_snap, &lg, 10000u, &c1, &bus1, &p1, &s_g1));
    /* Replay 2 -> c2/bus2 (independent state, same snap+log+target). */
    ASSERT_TRUE(tt_replay(&s_snap, &lg, 10000u, &c2, &bus2, &p2, &s_g2));

    /* TT-05: byte-equal cpu_t across both replays. */
    ASSERT_TRUE(c1.cycles == c2.cycles);
    ASSERT_TRUE(memcmp(&c1, &c2, sizeof c1) == 0);

    /* SRAM byte-equal. */
    region_t* r1 = bus_find_flat(&bus1, SRAM_BASE_ADDR);
    region_t* r2 = bus_find_flat(&bus2, SRAM_BASE_ADDR);
    ASSERT_TRUE(r1 != NULL && r2 != NULL);
    ASSERT_TRUE(memcmp(r1->buf, r2->buf, r1->size) == 0);

    ev_log_free(&lg);
}

/* Monotonicity: run_until_cycle never leaves c->cycles < target (within normal operation). */
TEST(tt_run_until_cycle_monotone) {
    cpu_t c; bus_t bus;
    systick_t st; nvic_t nv; scb_t scb;
    mpu_t mpu; dwt_t dwt; stm32_t stm32; eth_t eth; uart_t u;
    tt_periph_t p;
    setup_one(&c, &bus, &p, &st, &nv, &scb, &mpu, &dwt, &stm32, &eth, &u);

    u64 before = c.cycles;
    run_until_cycle(&c, &bus, 500u, &st, &scb, NULL, NULL, 0u, NULL, &p);
    ASSERT_TRUE(c.cycles >= 500u);
    ASSERT_TRUE(c.cycles >= before);
}

int main(void) {
    RUN(tt_replay_byte_equal);
    RUN(tt_run_until_cycle_monotone);
    TEST_REPORT();
}
