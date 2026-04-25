#ifndef CORTEX_M_DWT_H
#define CORTEX_M_DWT_H

#include "core/types.h"
#include "core/bus.h"

/* Data Watchpoint and Trace at 0xE0001000:
   +0x00 CTRL — bit 0 CYCCNTENA
   +0x04 CYCCNT — 32-bit cycle counter (free-running) */

#define DWT_BASE 0xE0001000u
#define DEMCR_BASE 0xE000EDFCu  /* TRCENA bit 24 */

typedef struct dwt_s {
    u32 ctrl;
    u32 cyccnt;
    u32 demcr;
} dwt_t;

int dwt_attach(bus_t* b, dwt_t* d);
void dwt_tick(dwt_t* d);

#endif
