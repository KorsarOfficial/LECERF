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

/* Same Thumb blob as test_tt_replay.c: SP=0x20040000, entry=0x8|1, NOPx3 then B. */
static const u8 k_blob[] = {
    0x00, 0x00, 0x04, 0x20,
    0x09, 0x00, 0x00, 0x00,
    0x00, 0xBF,
    0x00, 0xBF,
    0x00, 0xBF,
    0xFE, 0xE7,
};

extern cpu_t*  g_cpu_for_scb;
extern dwt_t*  g_dwt_for_run;
extern nvic_t* g_nvic_for_run;

static jit_t s_g1, s_g2;
static snap_blob_t s_snap;

static void setup_one(cpu_t* c, bus_t* bus, tt_periph_t* p,
                      systick_t* st, nvic_t* nv, scb_t* scb,
                      mpu_t* mpu, dwt_t* dwt, stm32_t* sm,
                      eth_t* e, uart_t* u) {
    bus_init(bus);
    bus_add_flat(bus, "flash", 0x00000000u, 1024u * 1024u, false);
    bus_add_flat(bus, "sram",  SRAM_BASE_ADDR, SRAM_SIZE, true);
    memset(u, 0, sizeof *u);
    uart_attach(bus, u);
    systick_attach(bus, st);
    scb_attach(bus, scb);
    g_cpu_for_scb = c;
    mpu_attach(bus, mpu);
    stm32_attach(bus, sm);
    sm->quiet = true;
    dwt_attach(bus, dwt);
    g_dwt_for_run = dwt;
    nvic_attach(bus, nv);
    g_nvic_for_run = nv;
    eth_attach(bus, e);
    /* Firmware-side rx buffer: pretend MAC driver wrote 0x20001000 into ETH.RX_ADDR.
       This is the destination eth_inject_rx will bus_write frame bytes into. */
    e->rx_addr = SRAM_BASE_ADDR + 0x1000u;
    bus_load_blob(bus, 0x00000000u, k_blob, (u32)sizeof k_blob);
    cpu_reset(c, CORE_M4);
    c->msp = bus_r32(bus, 0x0u);
    c->r[REG_SP] = c->msp;
    c->r[REG_PC] = bus_r32(bus, 0x4u) & ~1u;
    p->st = st; p->nv = nv; p->scb = scb;
    p->mpu = mpu; p->dwt = dwt; p->stm32 = sm;
    p->eth = e; p->uart = u;
}

/* TT-02 closure: ETH RX recorded into side store + replayed byte-equal twice. */
TEST(tt_eth_replay_byte_equal) {
    cpu_t c1, c2;
    bus_t bus1, bus2;
    systick_t st1, st2; nvic_t nv1, nv2; scb_t scb1, scb2;
    mpu_t mpu1, mpu2;   dwt_t dwt1, dwt2;
    stm32_t s1, s2;     eth_t e1, e2;     uart_t u1, u2;
    tt_periph_t p1, p2;

    memset(&s_g1, 0, sizeof s_g1);
    memset(&s_g2, 0, sizeof s_g2);

    setup_one(&c1, &bus1, &p1, &st1, &nv1, &scb1, &mpu1, &dwt1, &s1, &e1, &u1);
    setup_one(&c2, &bus2, &p2, &st2, &nv2, &scb2, &mpu2, &dwt2, &s2, &e2, &u2);

    /* tt_create allocates the side-blob store. The frame is recorded into
       g_tt->frames[0] and ev_log_append emits EVENT_ETH_RX with payload=0. */
    tt_t* tt = tt_create(10000u, 4u);
    ASSERT_TRUE(tt != NULL);

    /* Snapshot at cycle 0. */
    ASSERT_TRUE(snap_save(&s_snap, &c1, &bus1, &p1));

    /* Synthetic 64-byte frame: deterministic content (i ^ 0xA5). */
    u8 fr[64];
    for (u32 i = 0; i < sizeof fr; ++i) fr[i] = (u8)(i ^ 0xA5u);

    /* Record one ETH RX event at cycle 5000. */
    u32 fid = tt_record_eth_rx(5000ull, fr, sizeof fr);
    ASSERT_TRUE(fid == 0u);
    ASSERT_TRUE(tt->n_frames == 1u);
    ASSERT_TRUE(tt->log.n == 1u);
    ASSERT_TRUE(tt->log.buf[0].type == EVENT_ETH_RX);
    ASSERT_TRUE(tt->log.buf[0].payload == 0u);
    ASSERT_TRUE(tt->log.buf[0].cycle == 5000ull);

    /* Replay 1 -> c1/bus1. tt_replay sets g_replay_mode, restores snap, runs to target.
       The EVENT_ETH_RX event at cycle 5000 dispatches to eth_inject_rx via tt->frames[0]. */
    ASSERT_TRUE(tt_replay(&s_snap, &tt->log, 10000ull, &c1, &bus1, &p1, &s_g1));
    ASSERT_TRUE(c1.cycles >= 10000ull);

    /* Verify the frame actually landed in bus1 SRAM at rx_addr. */
    region_t* r1 = bus_find_flat(&bus1, SRAM_BASE_ADDR);
    ASSERT_TRUE(r1 != NULL);
    ASSERT_TRUE(memcmp(r1->buf + 0x1000u, fr, sizeof fr) == 0);
    ASSERT_TRUE(e1.rx_len == sizeof fr);
    ASSERT_TRUE((e1.status & 0x3u) == 0x3u);

    /* Replay 2 -> c2/bus2 (independent state, same snap+log+side_store+target). */
    ASSERT_TRUE(tt_replay(&s_snap, &tt->log, 10000ull, &c2, &bus2, &p2, &s_g2));

    /* TT-02 byte-eq: cpu_t identical across both replays. */
    ASSERT_TRUE(c1.cycles == c2.cycles);
    ASSERT_TRUE(memcmp(&c1, &c2, sizeof c1) == 0);

    /* SRAM byte-eq across both replays. */
    region_t* r2 = bus_find_flat(&bus2, SRAM_BASE_ADDR);
    ASSERT_TRUE(r2 != NULL);
    ASSERT_TRUE(memcmp(r1->buf, r2->buf, r1->size) == 0);

    /* Replay-mode guard: tt_record_eth_rx during replay must be no-op. */
    extern bool g_replay_mode;
    g_replay_mode = true;
    u32 nope = tt_record_eth_rx(20000ull, fr, sizeof fr);
    ASSERT_TRUE(nope == 0xFFFFFFFFu);
    ASSERT_TRUE(tt->log.n == 1u);  /* unchanged */
    g_replay_mode = false;

    tt_destroy(tt);
}

/* Capacity guard: appending TT_ETH_MAX+1 frames returns UINT32_MAX on the last call. */
TEST(tt_eth_capacity_guard) {
    tt_t* tt = tt_create(10000u, 1u);
    ASSERT_TRUE(tt != NULL);
    u8 fr[16] = {0};
    for (u32 i = 0; i < TT_ETH_MAX; ++i) {
        u32 id = tt_record_eth_rx((u64)i, fr, sizeof fr);
        ASSERT_TRUE(id == i);
    }
    u32 over = tt_record_eth_rx((u64)TT_ETH_MAX, fr, sizeof fr);
    ASSERT_TRUE(over == 0xFFFFFFFFu);
    ASSERT_TRUE(tt->n_frames == TT_ETH_MAX);
    tt_destroy(tt);
}

int main(void) {
    RUN(tt_eth_replay_byte_equal);
    RUN(tt_eth_capacity_guard);
    TEST_REPORT();
}
