#ifndef CORTEX_M_TT_H
#define CORTEX_M_TT_H

#include <stdio.h>
#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"
#include "core/nvic.h"
#include "core/jit.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "periph/mpu.h"
#include "periph/dwt.h"
#include "periph/stm32.h"
#include "periph/eth.h"
#include "periph/uart.h"

enum {
    EVENT_NONE       = 0,
    EVENT_UART_RX    = 1,
    EVENT_IRQ_INJECT = 2,
    EVENT_ETH_RX     = 3,
};

typedef struct ev_s {
    u64 cycle;
    u8  type;
    u8  pad[3];
    u32 payload;
} ev_t;

_Static_assert(sizeof(ev_t) == 16, "ev_t must be 16B");

#define TT_LOG_MAX 65536u

typedef struct ev_log_s {
    ev_t* buf;   /* malloc-owned, capacity = cap */
    u32   n;     /* used count */
    u32   cap;
    u32   pos;   /* replay cursor */
} ev_log_t;

typedef struct tt_periph_s {
    systick_t* st;
    nvic_t*    nv;
    scb_t*     scb;
    mpu_t*     mpu;
    dwt_t*     dwt;
    stm32_t*   stm32;
    eth_t*     eth;
    uart_t*    uart; /* used by 13-03 tt_inject_event for EVENT_UART_RX routing */
} tt_periph_t;

/* snap_blob_t forward decl (full def below in snapshot section). */
typedef struct snap_blob_s snap_blob_t;

/* snap_entry_t: parallel index entry for tt_t snap store. */
typedef struct snap_entry_s {
    u64 cycle;
    u32 snap_idx;
} snap_entry_t;

/* tt_t: full time-travel state machine. */
typedef struct tt_s {
    u32 stride;
    u32 max_snaps;
    snap_blob_t*  snaps;  /* malloc(max_snaps * sizeof(snap_blob_t)) */
    snap_entry_t* idx;    /* sorted by cycle, parallel array */
    u32 n_snaps;
    ev_log_t log;
} tt_t;

/* Event log */
void ev_log_init  (ev_log_t* lg, u32 cap);
void ev_log_free  (ev_log_t* lg);
bool ev_log_append(ev_log_t* lg, u64 cycle, u8 type, u32 payload);
u32  ev_log_seek  (const ev_log_t* lg, u64 cycle); /* lower_bound by cycle */

/* Weak record hooks; 13-04 provides strong overrides. */
void tt_record_irq    (u64 cycle, u8 irq);
void tt_record_uart_rx(u64 cycle, u8 byte);

/* Module-global tt pointer used by uart/nvic without threading tt through every call.
   NULL = no recording. Set by tt_create in 13-04. */
extern tt_t* g_tt;
extern bool  g_replay_mode; /* true during tt_replay; suppresses side effects */

/* ---- TT core lifecycle (13-04) ---- */

tt_t* tt_create    (u32 stride, u32 max_snaps);
void  tt_destroy   (tt_t* tt);

/* O(1): takes a snap iff cpu.cycles is on a stride boundary and budget not full. */
void  tt_on_cycle  (tt_t* tt, cpu_t* c, bus_t* bus, tt_periph_t* p);

/* Rewind: bsearch_le on snap idx, snap_restore, run_until_cycle to target. */
bool  tt_rewind    (tt_t* tt, u64 target, cpu_t* c, bus_t* bus, tt_periph_t* p, jit_t* g);

/* step_back(N): tt_rewind(cpu.cycles - N); whole-instruction granularity +/-1. */
bool  tt_step_back (tt_t* tt, u64 n,      cpu_t* c, bus_t* bus, tt_periph_t* p, jit_t* g);

/* diff: prints register + SRAM range-encoded deltas to FILE*. */
void  tt_diff      (const snap_blob_t* a, const snap_blob_t* b, FILE* out);

/* Wire jit_t for jit_reset_counters in snap_restore. */
void  tt_attach_jit(jit_t* g);

/* ---- Snapshot module (13-02) ---- */

#define SNAP_MAGIC   0x54544B30u  /* "TTK0" little-endian */
#define SNAP_VERSION 1u
#define SRAM_BASE_ADDR 0x20000000u
#define SRAM_SIZE      (256u * 1024u)

typedef struct snap_blob_s {
    u32 magic;
    u32 version;
    u64 cycle;
    cpu_t     cpu;
    systick_t st;
    nvic_t    nvic;
    scb_t     scb;
    mpu_t     mpu;
    dwt_t     dwt;
    stm32_t   stm32;
    eth_t     eth_state;  /* eth.bus zeroed on save, refilled on restore */
    uart_t    uart_state; /* rx_q/replay_mode preserved across snap */
    u32 sram_size;        /* always SRAM_SIZE */
    u8  sram[SRAM_SIZE];
    u32 checksum;         /* XOR32 of all preceding bytes */
} snap_blob_t;

bool snap_save   (snap_blob_t* blob, cpu_t* c, bus_t* bus, tt_periph_t* p);
bool snap_restore(const snap_blob_t* blob, cpu_t* c, bus_t* bus, tt_periph_t* p);

/* File round-trip (cross-session). */
bool snap_save_to_file  (const snap_blob_t* blob, const char* path);
bool snap_load_from_file(snap_blob_t* blob, const char* path);

/* Internal helper exposed for tests. */
u32  snap_xor32(const u8* data, u32 n);

/* Set by tt_create in 13-04; NULL until then. snap_restore flushes via jit_reset_counters. */
extern jit_t* g_jit_for_tt;

/* ---- Replay engine (13-03) ---- */

/* Dispatch one event: EVENT_UART_RX -> uart_inject_rx, EVENT_IRQ_INJECT -> nvic_set_pending. */
void tt_inject_event(cpu_t* c, bus_t* bus, tt_periph_t* p, const ev_t* e);

/* run_until_cycle: loops run_steps_full_g, drains log events at their cycle stamps,
   stops when c->cycles >= target_cycle (whole-instruction granularity;
   overshoot <= one ARM cycle). Returns cycles advanced. */
u64 run_until_cycle(cpu_t* c, bus_t* bus, u64 target_cycle,
                    systick_t* st, scb_t* scb, jit_t* g,
                    const ev_t* log, u32 log_n, u32* log_pos,
                    tt_periph_t* p);

/* tt_replay: snap_restore(start) then run_until_cycle to target_cycle under g_replay_mode.
   Returns true if restore succeeded. Byte-equal output for same (start, log, target). */
bool tt_replay(const snap_blob_t* start, const ev_log_t* log,
               u64 target_cycle, cpu_t* c, bus_t* bus, tt_periph_t* p,
               jit_t* g);

#endif
