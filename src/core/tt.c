#include "core/tt.h"
#include <stdlib.h>
#include <string.h>

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

/* No-op stubs; 13-04 replaces bodies with ev_log_append calls once g_tt is wired. */
void tt_record_irq(u64 cycle, u8 irq) {
    (void)cycle; (void)irq;
}

void tt_record_uart_rx(u64 cycle, u8 byte) {
    (void)cycle; (void)byte;
}
