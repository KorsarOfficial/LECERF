#include "core/tt.h"
#include "core/decoder.h"
#include "core/jit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

tt_t* g_tt          = NULL;
bool  g_replay_mode = false;

void ev_log_init(ev_log_t* lg, u32 cap) {
    lg->buf = (ev_t*)malloc((size_t)cap * sizeof(ev_t));
    lg->n   = 0;
    lg->cap = lg->buf ? cap : 0;
    lg->pos = 0;
}

void ev_log_free(ev_log_t* lg) {
    free(lg->buf);
    lg->buf = NULL;
    lg->n = lg->cap = lg->pos = 0;
}

bool ev_log_append(ev_log_t* lg, u64 cycle, u8 type, u32 payload) {
    if (lg->n >= lg->cap) {
        u32 nc = lg->cap ? lg->cap * 2 : 64;
        if (nc > TT_LOG_MAX) nc = TT_LOG_MAX;
        if (nc <= lg->cap) return false;
        ev_t* nb = (ev_t*)realloc(lg->buf, (size_t)nc * sizeof(ev_t));
        if (!nb) return false;
        lg->buf = nb;
        lg->cap = nc;
    }
    ev_t* e  = &lg->buf[lg->n++];
    e->cycle   = cycle;
    e->type    = type;
    e->pad[0]  = e->pad[1] = e->pad[2] = 0;
    e->payload = payload;
    return true;
}

/* Lower-bound: index of first entry with cycle >= target. */
u32 ev_log_seek(const ev_log_t* lg, u64 cycle) {
    u32 lo = 0, hi = lg->n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (lg->buf[mid].cycle < cycle) lo = mid + 1;
        else                            hi = mid;
    }
    return lo;
}

void tt_record_irq(u64 cycle, u8 irq) {
    if (g_tt && !g_replay_mode) ev_log_append(&g_tt->log, cycle, EVENT_IRQ_INJECT, (u32)irq);
}

void tt_record_uart_rx(u64 cycle, u8 byte) {
    if (g_tt && !g_replay_mode) ev_log_append(&g_tt->log, cycle, EVENT_UART_RX, (u32)byte);
}

u32 tt_record_eth_rx(u64 cyc, const u8* fr, u32 ln) {
    if (!g_tt || g_replay_mode || !fr || !ln) return 0xFFFFFFFFu;
    if (g_tt->n_frames >= TT_ETH_MAX) return 0xFFFFFFFFu;
    u32 cap = ln < TT_ETH_MTU ? ln : TT_ETH_MTU;
    u32 id  = g_tt->n_frames++;
    eth_frame_t* f = &g_tt->frames[id];
    f->len = cap;
    memcpy(f->buf, fr, cap);
    if (!ev_log_append(&g_tt->log, cyc, EVENT_ETH_RX, id)) {
        g_tt->n_frames--;
        return 0xFFFFFFFFu;
    }
    return id;
}

/* ---- Snapshot module (13-02) ---- */

jit_t* g_jit_for_tt = NULL;

u32 snap_xor32(const u8* d, u32 n) {
    u32 a = 0u, i = 0u;
    for (; i + 4u <= n; i += 4u) {
        u32 w; memcpy(&w, d + i, 4u); a ^= w;
    }
    for (; i < n; ++i) a ^= ((u32)d[i]) << ((i & 3u) * 8u);
    return a;
}

bool snap_save(snap_blob_t* b, cpu_t* c, bus_t* bus, tt_periph_t* p) {
    if (!b || !c || !bus || !p) return false;
    region_t* sr = bus_find_flat(bus, SRAM_BASE_ADDR);
    if (!sr || sr->size < SRAM_SIZE) return false;
    b->magic      = SNAP_MAGIC;
    b->version    = SNAP_VERSION;
    b->cycle      = c->cycles;
    b->cpu        = *c;
    b->st         = *p->st;
    b->nvic       = *p->nv;
    b->scb        = *p->scb;
    b->scb.ctx    = NULL;   /* don't serialize per-board back-pointer */
    b->mpu        = *p->mpu;
    b->dwt        = *p->dwt;
    b->stm32      = *p->stm32;
    b->eth_state  = *p->eth;
    b->eth_state.bus = NULL;  /* don't serialize back-pointer */
    b->uart_state          = *p->uart;
    b->uart_state.sink     = NULL;   /* don't serialize TX sink callback */
    b->uart_state.sink_ctx = NULL;   /* don't serialize TX sink context */
    b->sram_size  = SRAM_SIZE;
    memcpy(b->sram, sr->buf, SRAM_SIZE);
    b->checksum   = 0u;
    b->checksum   = snap_xor32((const u8*)b, (u32)(sizeof(*b) - sizeof(u32)));
    return true;
}

bool snap_restore(const snap_blob_t* b, cpu_t* c, bus_t* bus, tt_periph_t* p) {
    if (!b || !c || !bus || !p) return false;
    if (b->magic != SNAP_MAGIC || b->version != SNAP_VERSION) return false;
    if (b->sram_size != SRAM_SIZE) return false;
    snap_blob_t tmp = *b;
    u32 saved = tmp.checksum;
    tmp.checksum = 0u;
    if (snap_xor32((const u8*)&tmp, (u32)(sizeof(tmp) - sizeof(u32))) != saved) return false;
    region_t* sr = bus_find_flat(bus, SRAM_BASE_ADDR);
    if (!sr) return false;
    *c        = b->cpu;
    *p->st    = b->st;
    *p->nv    = b->nvic;
    { void* saved_ctx = p->scb->ctx; *p->scb = b->scb; p->scb->ctx = saved_ctx; } /* fix ctx back-pointer */
    *p->mpu   = b->mpu;
    *p->dwt   = b->dwt;
    *p->stm32 = b->stm32;
    *p->eth   = b->eth_state;
    p->eth->bus = bus;  /* fix back-pointer */
    { /* restore uart but preserve TX sink (not serialized) */
      int (*saved_sink)(void*,int) = p->uart->sink;
      void* saved_sink_ctx = p->uart->sink_ctx;
      *p->uart = b->uart_state;
      p->uart->sink     = saved_sink;
      p->uart->sink_ctx = saved_sink_ctx;
    }
    memcpy(sr->buf, b->sram, SRAM_SIZE);
    run_dcache_invalidate();
    /* TT safety: after snap_restore, all compiled TBs reference the pre-restore
       PC layout. Flush full cache; TBs will recompile lazily as PCs heat up. */
    jit_flush(g_jit_for_tt);
    return true;
}

bool snap_save_to_file(const snap_blob_t* b, const char* path) {
    if (!b || !path) return false;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t n = fwrite(b, 1u, sizeof(*b), f);
    fclose(f);
    return n == sizeof(*b);
}

bool snap_load_from_file(snap_blob_t* b, const char* path) {
    if (!b || !path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(b, 1u, sizeof(*b), f);
    fclose(f);
    if (n != sizeof(*b)) return false;
    if (b->magic != SNAP_MAGIC || b->version != SNAP_VERSION) return false;
    snap_blob_t tmp = *b;
    u32 saved = tmp.checksum;
    tmp.checksum = 0u;
    return snap_xor32((const u8*)&tmp, (u32)(sizeof(tmp) - sizeof(u32))) == saved;
}

/* ---- Replay engine (13-03) ---- */

#include "core/run.h"

void tt_inject_event(cpu_t* c, bus_t* bus, tt_periph_t* p, const ev_t* e) {
    (void)c; (void)bus;
    if (!p || !e) return;
    switch (e->type) {
        case EVENT_UART_RX:
            if (p->uart) uart_inject_rx(p->uart, (u8)(e->payload & 0xFFu));
            break;
        case EVENT_IRQ_INJECT:
            if (p->nv) nvic_set_pending(p->nv, e->payload);
            break;
        case EVENT_ETH_RX: {
            if (!p->eth || !g_tt) break;
            u32 id = e->payload;
            if (id >= g_tt->n_frames) break;
            const eth_frame_t* fr = &g_tt->frames[id];
            if (!fr->len) break;
            eth_inject_rx(p->eth, fr->buf, fr->len);
            break;
        }
        default: break;
    }
}

bool tt_replay(const snap_blob_t* s, const ev_log_t* lg, u64 target,
               cpu_t* c, bus_t* bus, tt_periph_t* p, jit_t* g) {
    if (!s || !c || !bus || !p) return false;
    bool prev = g_replay_mode;
    g_replay_mode = true;
    if (!snap_restore(s, c, bus, p)) { g_replay_mode = prev; return false; }
    if (target < c->cycles) { g_replay_mode = prev; return false; }
    u32 pos = lg ? ev_log_seek(lg, c->cycles) : 0u;
    const ev_t* buf = lg ? lg->buf : NULL;
    u32 n = lg ? lg->n : 0u;
    run_until_cycle(c, bus, target, p->st, p->scb, g, buf, n, &pos, p);
    g_replay_mode = prev;
    return true;
}

/* ---- TT core lifecycle (13-04) ---- */

tt_t* tt_create(u32 stride, u32 max_snaps) {
    if (!stride || !max_snaps) return NULL;
    tt_t* tt = (tt_t*)calloc(1, sizeof(tt_t));
    if (!tt) return NULL;
    tt->stride    = stride;
    tt->max_snaps = max_snaps;
    tt->snaps = (snap_blob_t*)calloc(max_snaps, sizeof(snap_blob_t));
    tt->idx   = (snap_entry_t*)calloc(max_snaps, sizeof(snap_entry_t));
    if (!tt->snaps || !tt->idx) { tt_destroy(tt); return NULL; }
    ev_log_init(&tt->log, 16u);
    tt->frames   = (eth_frame_t*)calloc(TT_ETH_MAX, sizeof(eth_frame_t));
    tt->n_frames = 0u;
    if (!tt->frames) { tt_destroy(tt); return NULL; }
    g_tt = tt;
    return tt;
}

void tt_destroy(tt_t* tt) {
    if (!tt) return;
    if (g_tt == tt) g_tt = NULL;
    free(tt->frames);
    free(tt->snaps);
    free(tt->idx);
    ev_log_free(&tt->log);
    free(tt);
}

void tt_on_cycle(tt_t* tt, cpu_t* c, bus_t* bus, tt_periph_t* p) {
    if (!tt || !c) return;
    if (g_replay_mode) return;
    /* snap when cycles cross the next stride boundary (not strict modulo:
       run_steps_full_g runs N instructions, not N cycles, so c->cycles may
       overshoot a boundary by a few cycles). */
    u64 next = (tt->n_snaps == 0u)
               ? (u64)tt->stride
               : tt->idx[tt->n_snaps - 1u].cycle + (u64)tt->stride;
    if (c->cycles < next) return;
    if (tt->n_snaps >= tt->max_snaps) return;
    snap_blob_t* b = &tt->snaps[tt->n_snaps];
    if (!snap_save(b, c, bus, p)) return;
    tt->idx[tt->n_snaps].cycle    = c->cycles;
    tt->idx[tt->n_snaps].snap_idx = tt->n_snaps;
    tt->n_snaps++;
}

void tt_attach_jit(jit_t* g) {
    g_jit_for_tt = g;
}

/* lower_bound: largest i s.t. idx[i].cycle <= target; -1 if none. */
static int tt_bsearch_le(const snap_entry_t* a, u32 n, u64 target) {
    int lo = 0, hi = (int)n - 1, best = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid].cycle <= target) { best = mid; lo = mid + 1; }
        else                        hi = mid - 1;
    }
    return best;
}

bool tt_rewind(tt_t* tt, u64 target, cpu_t* c, bus_t* bus, tt_periph_t* p, jit_t* g) {
    if (!tt || !c || !bus || !p) return false;
    int i = tt_bsearch_le(tt->idx, tt->n_snaps, target);
    if (i < 0) return false;
    bool prev = g_replay_mode;
    g_replay_mode = true;
    if (!snap_restore(&tt->snaps[tt->idx[i].snap_idx], c, bus, p)) {
        g_replay_mode = prev; return false;
    }
    if (target > c->cycles) {
        u32 pos = ev_log_seek(&tt->log, c->cycles);
        run_until_cycle(c, bus, target, p->st, p->scb, g,
                        tt->log.buf, tt->log.n, &pos, p);
    }
    g_replay_mode = prev;
    return true;
}

bool tt_step_back(tt_t* tt, u64 n, cpu_t* c, bus_t* bus, tt_periph_t* p, jit_t* g) {
    if (!tt || !c) return false;
    if (n > c->cycles) return false;
    return tt_rewind(tt, c->cycles - n, c, bus, p, g);
}

void tt_diff(const snap_blob_t* a, const snap_blob_t* b, FILE* out) {
    if (!a || !b || !out) return;
    if (a->cycle != b->cycle)
        fprintf(out, "cycle: %llu -> %llu\n",
                (unsigned long long)a->cycle, (unsigned long long)b->cycle);
    for (int i = 0; i < 16; ++i)
        if (a->cpu.r[i] != b->cpu.r[i])
            fprintf(out, "R%d: 0x%08x -> 0x%08x\n", i, a->cpu.r[i], b->cpu.r[i]);
    if (a->cpu.apsr != b->cpu.apsr)
        fprintf(out, "APSR: 0x%08x -> 0x%08x\n", a->cpu.apsr, b->cpu.apsr);
    u32 sz = a->sram_size < b->sram_size ? a->sram_size : b->sram_size;
    u32 j  = 0u;
    while (j < sz) {
        if (a->sram[j] != b->sram[j]) {
            u32 k = j;
            while (k < sz && a->sram[k] != b->sram[k]) k++;
            fprintf(out, "SRAM[0x%08x..0x%08x]: %u bytes differ\n",
                    SRAM_BASE_ADDR + j, SRAM_BASE_ADDR + k - 1u, k - j);
            j = k;
        } else { j++; }
    }
}
