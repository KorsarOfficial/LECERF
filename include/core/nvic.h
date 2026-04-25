#ifndef CORTEX_M_NVIC_H
#define CORTEX_M_NVIC_H

#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"

/* Cortex-M exception numbers (ARM ARM B1.5.2):
   1   Reset
   2   NMI
   3   HardFault
   11  SVCall
   14  PendSV
   15  SysTick
   16+ IRQ0..IRQn */

#define EXC_RESET      1
#define EXC_NMI        2
#define EXC_HARD_FAULT 3
#define EXC_MEM_FAULT  4
#define EXC_BUS_FAULT  5
#define EXC_USAGE_FAULT 6
#define EXC_SVC        11
#define EXC_DEBUG_MON  12
#define EXC_PENDSV     14
#define EXC_SYSTICK    15
#define EXC_IRQ0       16
#define NVIC_IRQ_LINES 240
#define NVIC_BASE      0xE000E100u
#define NVIC_SIZE      0x500u

typedef struct nvic_s {
    /* 240 IRQ lines, packed as 8 × 32-bit ISER/ICER. */
    u32 enable[8];   /* NVIC_ISER0..7 / NVIC_ICER0..7 */
    u32 pending[8];  /* NVIC_ISPR0..7 / NVIC_ICPR0..7 */
    u32 active[8];
    u8  prio[NVIC_IRQ_LINES];
} nvic_t;

struct bus_s;
int  nvic_attach(struct bus_s* b, nvic_t* n);
void nvic_set_pending    (nvic_t* n, u32 irq);
/* External injection path: logs EVENT_IRQ_INJECT when g_tt is set. */
void nvic_set_pending_ext(nvic_t* n, u32 irq, u64 cycle);
int  nvic_pick(const nvic_t* n);   /* returns lowest-numbered pending+enabled IRQ, -1 */
void nvic_clear_pending(nvic_t* n, u32 irq);
void nvic_set_active(nvic_t* n, u32 irq);
void nvic_clear_active(nvic_t* n, u32 irq);

/* Stack frame pushed on exception entry (ARM ARM B1.5.6):
   [SP+0x00] R0
   [SP+0x04] R1
   [SP+0x08] R2
   [SP+0x0C] R3
   [SP+0x10] R12
   [SP+0x14] LR
   [SP+0x18] return addr
   [SP+0x1C] xPSR */

/* Trigger exception entry: save context, set PC to handler from VTOR[exc]. */
bool exc_enter(cpu_t* c, bus_t* b, u8 exc);
/* Raise a fault. If the fault's handler is unavailable or escalation rules
   require it, escalate to HardFault. Sets CFSR/HFSR/MMFAR/BFAR as needed. */
void raise_fault(cpu_t* c, bus_t* b, u8 fault, u32 fault_addr, u32 status_bit);
/* EXC_RETURN value execution: restore context, pop stack, return to thread. */
bool exc_return(cpu_t* c, bus_t* b, u32 exc_return);

#endif
