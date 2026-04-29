#include "core/board.h"
#include "core/run.h"
#include <stdlib.h>
#include <string.h>

/* ---- Profile table ---- */

static const board_profile_t BOARDS[] = {
    { "stm32f103",  0x00000000u,  128u*1024u, 0x20000000u,  20u*1024u, CORE_M3, 0u },
    { "stm32f407",  0x00000000u,  512u*1024u, 0x20000000u, 128u*1024u, CORE_M4, 0u },
    { "generic-m4", 0x00000000u, 1024u*1024u, 0x20000000u, 256u*1024u, CORE_M4, 0u },
    { NULL, 0u, 0u, 0u, 0u, CORE_M4, 0u }
};

const board_profile_t* board_profile_find(const char* name) {
    if (!name) return NULL;
    for (const board_profile_t* p = BOARDS; p->name; ++p)
        if (strcmp(p->name, name) == 0) return p;
    return NULL;
}

/* ---- UART capture sink ---- */

static int uart_capture(void* ctx, int c) {
    board_t* b = (board_t*)ctx;
    if (b->uart_buf_n < b->uart_buf_cap)
        b->uart_buf[b->uart_buf_n++] = (u8)c;
    return c;
}

/* ---- board_create / board_destroy ---- */

board_t* board_create(const char* name) {
    const board_profile_t* prof = board_profile_find(name);
    if (!prof) return NULL;

    board_t* b = (board_t*)calloc(1u, sizeof(board_t));
    if (!b) return NULL;

    b->prof = prof;

    /* Heap-alloc JIT (~2 MB) */
    b->jit = (jit_t*)calloc(1u, sizeof(jit_t));
    if (!b->jit) { free(b); return NULL; }
    jit_init(b->jit);

    /* UART capture buffer (64 KB) */
    b->uart_buf_cap = 65536u;
    b->uart_buf = (u8*)malloc(b->uart_buf_cap);
    if (!b->uart_buf) { free(b->jit); free(b); return NULL; }
    b->uart_buf_n = 0u;

    /* Bus + memory regions */
    bus_init(&b->bus);
    if (bus_add_flat(&b->bus, "flash", prof->flash_base, prof->flash_size, false) < 0 ||
        bus_add_flat(&b->bus, "sram",  prof->sram_base,  prof->sram_size,  true)  < 0) {
        free(b->uart_buf); free(b->jit); free(b);
        return NULL;
    }

    /* Attach peripherals */
    b->uart.sink     = uart_capture;
    b->uart.sink_ctx = b;
    uart_attach(&b->bus, &b->uart);
    systick_attach(&b->bus, &b->st);
    scb_attach(&b->bus, &b->scb);
    b->scb.ctx = &b->cpu;          /* wire ctx AFTER scb_attach zeros the struct */
    mpu_attach(&b->bus, &b->mpu);
    b->stm32.quiet = true;
    stm32_attach(&b->bus, &b->stm32);
    dwt_attach(&b->bus, &b->dwt);
    nvic_attach(&b->bus, &b->nvic);
    eth_attach(&b->bus, &b->eth);

    /* CPU reset */
    cpu_reset(&b->cpu, prof->core);

    /* No g_* touched; leave legacy globals at NULL/false. */
    return b;
}

void board_destroy(board_t* b) {
    if (!b) return;
    jit_flush(b->jit);
    free(b->jit);
    free(b->uart_buf);
    if (b->tt) tt_destroy(b->tt);
    /* Free flat bus buffers (calloc'd by bus_add_flat) */
    for (u32 i = 0; i < b->bus.n; ++i) {
        region_t* r = &b->bus.regs[i];
        if (r->kind == REGION_FLAT) { free(r->buf); r->buf = NULL; }
    }
    free(b);
}

/* ---- board_flash ---- */

bool board_flash(board_t* b, const u8* data, u32 sz) {
    if (!b || !data || !sz) return false;
    if (!bus_load_blob(&b->bus, b->prof->flash_base, data, sz)) return false;
    b->cpu.msp     = bus_r32(&b->bus, b->prof->flash_base);
    b->cpu.r[REG_SP] = b->cpu.msp;
    u32 entry = bus_r32(&b->bus, b->prof->flash_base + 4u) & ~1u;
    b->cpu.r[REG_PC] = entry ? entry : b->prof->flash_base;
    b->cpu.halted = false;
    return true;
}

/* ---- board_run ---- */

u64 board_run(board_t* b, u64 max_steps, int* exit_cause) {
    if (!b) { if (exit_cause) *exit_cause = BOARD_FAULT; return 0u; }

    run_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cpu    = &b->cpu;
    ctx.bus    = &b->bus;
    ctx.nvic   = &b->nvic;
    ctx.st     = &b->st;
    ctx.scb    = &b->scb;
    ctx.dwt    = &b->dwt;
    ctx.jit    = b->jit;
    ctx.tt     = b->tt;
    ctx.replay = false;

    bus_set_cookie(&b->bus, &ctx);
    u64 n = run_steps_full_gc(&ctx, max_steps);
    bus_set_cookie(&b->bus, NULL);

    int cause = BOARD_TIMEOUT;
    if (b->cpu.halted)                        cause = BOARD_HALT;
    else if (b->cpu.cfsr | b->cpu.hfsr)       cause = BOARD_FAULT;
    if (exit_cause) *exit_cause = cause;
    return n;
}

/* ---- board_uart_drain ---- */

u32 board_uart_drain(board_t* b, u8* dst, u32 cap) {
    if (!b || !dst || !cap) return 0u;
    u32 n = b->uart_buf_n < cap ? b->uart_buf_n : cap;
    memcpy(dst, b->uart_buf, n);
    b->uart_buf_n = 0u;
    return n;
}

/* ---- board_gpio_get ---- */

int board_gpio_get(const board_t* b, u32 port, u32 pin) {
    if (!b || pin >= 16u) return 0;
    switch (port) {
        case 0: return (int)((b->stm32.odr_a >> pin) & 1u);
        case 1: return (int)((b->stm32.odr_b >> pin) & 1u);
        case 2: return (int)((b->stm32.odr_c >> pin) & 1u);
        default: return 0;
    }
}

/* ---- board_cpu_reg ---- */

u32 board_cpu_reg(const board_t* b, u32 n) {
    if (!b) return 0u;
    if (n < 16u) return b->cpu.r[n];
    if (n == 16u) return b->cpu.apsr;
    return 0u;
}

/* ---- board_cycles ---- */

u64 board_cycles(const board_t* b) {
    return b ? b->cpu.cycles : 0u;
}

/* ---- board_enable_timetravel ---- */

void board_enable_timetravel(board_t* b, u32 stride, u32 max_snaps) {
    if (!b || b->tt) return;   /* already enabled */
    /* tt_create would set g_tt; use tt_create_local to avoid that.
       For now, allocate manually (same as tt_create but skip g_tt assignment). */
    if (!stride || !max_snaps) return;
    tt_t* tt = (tt_t*)calloc(1u, sizeof(tt_t));
    if (!tt) return;
    tt->stride    = stride;
    tt->max_snaps = max_snaps;
    tt->snaps  = (snap_blob_t*)calloc(max_snaps, sizeof(snap_blob_t));
    tt->idx    = (snap_entry_t*)calloc(max_snaps, sizeof(snap_entry_t));
    if (!tt->snaps || !tt->idx) { tt_destroy(tt); return; }
    ev_log_init(&tt->log, 16u);
    tt->frames   = (eth_frame_t*)calloc(TT_ETH_MAX, sizeof(eth_frame_t));
    tt->n_frames = 0u;
    if (!tt->frames) { tt_destroy(tt); return; }
    /* Do NOT set g_tt: this is a per-board tt. */
    b->tt = tt;
}

/* ---- board_inject_irq ---- */

void board_inject_irq(board_t* b, u32 irq) {
    if (!b) return;
    if (b->tt && !b->tt->log.buf) ev_log_init(&b->tt->log, 16u);
    if (b->tt) ev_log_append(&b->tt->log, b->cpu.cycles, EVENT_IRQ_INJECT, irq);
    nvic_set_pending(&b->nvic, irq);
}

/* ---- board_get_ev_log_count ---- */

u32 board_get_ev_log_count(const board_t* b) {
    return b && b->tt ? b->tt->log.n : 0u;
}
