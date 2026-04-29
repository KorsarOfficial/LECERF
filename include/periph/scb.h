#ifndef CORTEX_M_SCB_H
#define CORTEX_M_SCB_H

#include "core/types.h"
#include "core/bus.h"

/* System Control Block at 0xE000ED00.
   +0x00 CPUID
   +0x04 ICSR   — bit 28 PENDSVSET, bit 27 PENDSVCLR, bit 26 PENDSTSET, bit 25 PENDSTCLR,
                  bit 31 NMIPENDSET, bit 22 ISRPENDING, bits[11:0] VECTPENDING
   +0x08 VTOR
   +0x0C AIRCR
   +0x10 SCR
   +0x14 CCR
   ... */

#define SCB_BASE 0xE000ED00u
#define SCB_SIZE 0x100u

typedef struct scb_s {
    u32 vtor;
    u32 aircr;
    u32 ccr;
    bool pendsv_pending;
    bool systick_pending_manual;
    void* ctx;    /* per-board cpu_t*; preferred over g_cpu_for_scb */
} scb_t;

int scb_attach(bus_t* b, scb_t* s);

#endif
