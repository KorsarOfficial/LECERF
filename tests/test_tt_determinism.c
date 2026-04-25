#include "test_harness.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/nvic.h"
#include "core/jit.h"
#include "core/run.h"
#include "core/tt.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include <string.h>

/* Minimal Thumb-2 blob: 3 NOPs then infinite branch loop at offset 6.
   SP=0x20010000 (word 0), entry=0x00000009|1 (word 1, Thumb bit set).
   Layout (little-endian 32-bit words then Thumb16 insns):
     [0x00000000] = 0x20010000  (initial SP)
     [0x00000004] = 0x00000009  (reset handler = 0x8 | 1 Thumb)
     [0x00000008] = 0xBF00      NOP (Thumb16)
     [0x0000000A] = 0xBF00      NOP
     [0x0000000C] = 0xBF00      NOP
     [0x0000000E] = 0xE7FE      B . (infinite loop) */
static const u8 k_blob[] = {
    0x00, 0x00, 0x01, 0x20,  /* initial SP */
    0x09, 0x00, 0x00, 0x00,  /* reset handler Thumb addr */
    0x00, 0xBF,              /* NOP */
    0x00, 0xBF,              /* NOP */
    0x00, 0xBF,              /* NOP */
    0xFE, 0xE7,              /* B . */
};

#define FLASH_BASE 0x00000000u
#define FLASH_SIZE (1u << 20)
#define SRAM_BASE  0x20000000u
#define TEST_SRAM_SIZE (64u << 10)

static void run_one(cpu_t* c, bus_t* b, u64 n) {
    bus_init(b);
    bus_add_flat(b, "flash", FLASH_BASE, FLASH_SIZE, false);
    bus_add_flat(b, "sram",  SRAM_BASE,  TEST_SRAM_SIZE, true);
    bus_load_blob(b, FLASH_BASE, k_blob, (u32)sizeof(k_blob));

    cpu_reset(c, CORE_M4);
    c->msp = bus_r32(b, 0x0);
    c->r[REG_SP] = c->msp;
    u32 entry = bus_r32(b, 0x4) & ~1u;
    c->r[REG_PC] = entry;

    /* NULL jit: interp-only path, fully deterministic, no JIT counter state. */
    run_steps_full_g(c, b, n, NULL, NULL, NULL);
}

static jit_t s_ga, s_gb; /* jit_t ~2MB each; must not be stack-allocated */

TEST(tt_two_run_equal) {
    cpu_t a, b_cpu; bus_t ba, bb;
    jit_t* ga = &s_ga; memset(ga, 0, sizeof *ga);
    jit_t* gb = &s_gb; memset(gb, 0, sizeof *gb);

    run_one(&a,     &ba, 10000);
    run_one(&b_cpu, &bb, 10000);

    /* cpu_t byte-equal: TT-01 smoke test */
    ASSERT_TRUE(memcmp(&a, &b_cpu, sizeof a) == 0);

    /* SRAM regions byte-equal */
    region_t* ra = bus_find_flat(&ba, SRAM_BASE);
    region_t* rb = bus_find_flat(&bb, SRAM_BASE);
    ASSERT_TRUE(ra != NULL && rb != NULL);
    ASSERT_TRUE(memcmp(ra->buf, rb->buf, ra->size) == 0);

    /* jit counters equal after explicit reset */
    jit_reset_counters(ga);
    jit_reset_counters(gb);
    ASSERT_TRUE(memcmp(ga->counters, gb->counters, sizeof ga->counters) == 0);
}

int main(void) {
    RUN(tt_two_run_equal);
    TEST_REPORT();
}
