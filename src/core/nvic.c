#include "core/nvic.h"
#include "core/tt.h"

/* === NVIC peripheral MMIO (ARM ARM B3.4) === */
static u32 nvic_read(void* ctx, addr_t off, u32 size) {
    (void)size;
    nvic_t* n = (nvic_t*)ctx;
    if (off < 0x80) return n->enable[(off >> 2) & 7];
    if (off < 0x100) return n->enable[((off - 0x80) >> 2) & 7];
    if (off < 0x180) return n->pending[((off - 0x100) >> 2) & 7];
    if (off < 0x200) return n->pending[((off - 0x180) >> 2) & 7];
    if (off < 0x280) return n->active[((off - 0x200) >> 2) & 7];
    if (off >= 0x300 && off < 0x300 + NVIC_IRQ_LINES) return n->prio[off - 0x300];
    return 0;
}
static void nvic_write(void* ctx, addr_t off, u32 v, u32 size) {
    (void)size;
    nvic_t* n = (nvic_t*)ctx;
    if (off < 0x80)        n->enable[(off >> 2) & 7] |= v;
    else if (off < 0x100)  n->enable[((off - 0x80) >> 2) & 7] &= ~v;
    else if (off < 0x180)  n->pending[((off - 0x100) >> 2) & 7] |= v;
    else if (off < 0x200)  n->pending[((off - 0x180) >> 2) & 7] &= ~v;
    else if (off >= 0x300 && off < 0x300 + NVIC_IRQ_LINES) n->prio[off - 0x300] = (u8)v;
}
void nvic_set_pending(nvic_t* n, u32 irq) {
    if (irq < NVIC_IRQ_LINES) n->pending[irq >> 5] |= (1u << (irq & 31));
}

void nvic_set_pending_ext(nvic_t* n, u32 irq, u64 cycle) {
    if (g_tt && !g_replay_mode) tt_record_irq(cycle, (u8)irq);
    nvic_set_pending(n, irq);
}
void nvic_clear_pending(nvic_t* n, u32 irq) {
    if (irq < NVIC_IRQ_LINES) n->pending[irq >> 5] &= ~(1u << (irq & 31));
}
void nvic_set_active(nvic_t* n, u32 irq) {
    if (irq < NVIC_IRQ_LINES) n->active[irq >> 5] |= (1u << (irq & 31));
}
void nvic_clear_active(nvic_t* n, u32 irq) {
    if (irq < NVIC_IRQ_LINES) n->active[irq >> 5] &= ~(1u << (irq & 31));
}
int nvic_pick(const nvic_t* n) {
    int best = -1;
    u8 best_prio = 0xFF;
    for (u32 w = 0; w < 8; ++w) {
        u32 ready = n->enable[w] & n->pending[w] & ~n->active[w];
        while (ready) {
            u32 b = __builtin_ctz(ready);
            ready &= ready - 1;
            u32 irq = w * 32 + b;
            if (n->prio[irq] < best_prio) { best_prio = n->prio[irq]; best = (int)irq; }
        }
    }
    return best;
}
int nvic_attach(struct bus_s* b, nvic_t* n) {
    for (u32 i = 0; i < 8; ++i) { n->enable[i] = n->pending[i] = n->active[i] = 0; }
    for (u32 i = 0; i < NVIC_IRQ_LINES; ++i) n->prio[i] = 0;
    return bus_add_mmio(b, "nvic", NVIC_BASE, NVIC_SIZE, n, nvic_read, nvic_write);
}

/* Cortex-M exception entry / exit with MSP/PSP selection.
   ARM ARM B1.5.6 / B1.5.7. */

bool exc_enter(cpu_t* c, bus_t* b, u8 exc) {
    /* Determine stack to push on: if in Thread mode and CONTROL.SPSEL=1, use PSP;
       otherwise MSP. Also sync current SP back to MSP/PSP. */
    bool use_psp = (c->mode == MODE_THREAD) && ((c->control & 2u) != 0);
    addr_t sp;
    if (use_psp) { c->psp = c->r[REG_SP]; sp = c->psp; }
    else         { c->msp = c->r[REG_SP]; sp = c->msp; }

    sp -= 32;
    addr_t p = sp;

    u32 xpsr = (c->apsr & 0xF8000000u) | (c->epsr & 0x0600FC00u) | ((u32)exc & 0x1FFu);

    if (!bus_write(b, p + 0x00, 4, c->r[REG_R0]))  return false;
    if (!bus_write(b, p + 0x04, 4, c->r[REG_R1]))  return false;
    if (!bus_write(b, p + 0x08, 4, c->r[REG_R2]))  return false;
    if (!bus_write(b, p + 0x0C, 4, c->r[REG_R3]))  return false;
    if (!bus_write(b, p + 0x10, 4, c->r[REG_R12])) return false;
    if (!bus_write(b, p + 0x14, 4, c->r[REG_LR]))  return false;
    if (!bus_write(b, p + 0x18, 4, c->r[REG_PC]))  return false;
    if (!bus_write(b, p + 0x1C, 4, xpsr))           return false;

    if (use_psp) c->psp = sp;
    else         c->msp = sp;

    /* After entry, handler always runs on MSP (Handler mode). */
    c->mode = MODE_HANDLER;
    c->r[REG_SP] = c->msp;

    /* EXC_RETURN: bits[3:0] = 1001 return to thread with MSP,
       1101 return to thread with PSP, 0001 return to handler. */
    u32 exc_ret;
    if (use_psp) exc_ret = 0xFFFFFFFDu;
    else         exc_ret = 0xFFFFFFF9u;
    c->r[REG_LR] = exc_ret;

    /* Jump to handler from vector table. */
    u32 handler = 0;
    if (!bus_read(b, 4u * (u32)exc, 4, &handler)) return false;
    c->r[REG_PC] = handler & ~1u;
    c->ipsr = exc;
    c->itstate = 0;
    return true;
}

extern nvic_t* g_nvic_for_run;

/* Raise a configurable fault. Sets fault status bits, then escalates to
   HardFault if we are already in handler mode (no nesting in our model). */
void raise_fault(cpu_t* c, bus_t* b, u8 fault, u32 fault_addr, u32 status_bit) {
    /* Map fault → CFSR field. CFSR layout (ARM ARM B3.2.15):
       bits[7:0] MMFSR, bits[15:8] BFSR, bits[31:16] UFSR */
    if (fault == EXC_MEM_FAULT) {
        c->cfsr |= status_bit & 0xFFu;
        if (status_bit & 0x80) c->mmfar = fault_addr;
    } else if (fault == EXC_BUS_FAULT) {
        c->cfsr |= (status_bit & 0xFFu) << 8;
        if (status_bit & 0x80) c->bfar = fault_addr;
    } else if (fault == EXC_USAGE_FAULT) {
        c->cfsr |= (status_bit & 0xFFFFu) << 16;
    }
    /* Escalate to HardFault if we're already in a handler. */
    u8 target = fault;
    if (c->mode == MODE_HANDLER) {
        c->hfsr |= (1u << 30);  /* FORCED */
        target = EXC_HARD_FAULT;
    }
    exc_enter(c, b, target);
}

bool exc_return(cpu_t* c, bus_t* b, u32 exc_return) {
    /* Clear active bit for the IRQ we are returning from. */
    if (g_nvic_for_run && c->ipsr >= EXC_IRQ0 && c->ipsr < EXC_IRQ0 + NVIC_IRQ_LINES) {
        nvic_clear_active(g_nvic_for_run, c->ipsr - EXC_IRQ0);
    }
    /* Select where to pop from. */
    bool to_psp = (exc_return & 0xFu) == 0xDu;
    bool to_handler = (exc_return & 0xFu) == 0x1u;

    addr_t sp = to_psp ? c->psp : c->msp;

    u32 r0, r1, r2, r3, r12, lr, pc, xpsr;
    if (!bus_read(b, sp + 0x00, 4, &r0))  return false;
    if (!bus_read(b, sp + 0x04, 4, &r1))  return false;
    if (!bus_read(b, sp + 0x08, 4, &r2))  return false;
    if (!bus_read(b, sp + 0x0C, 4, &r3))  return false;
    if (!bus_read(b, sp + 0x10, 4, &r12)) return false;
    if (!bus_read(b, sp + 0x14, 4, &lr))  return false;
    if (!bus_read(b, sp + 0x18, 4, &pc))  return false;
    if (!bus_read(b, sp + 0x1C, 4, &xpsr))return false;
    c->r[REG_R0] = r0;  c->r[REG_R1] = r1;
    c->r[REG_R2] = r2;  c->r[REG_R3] = r3;
    c->r[REG_R12] = r12;
    c->r[REG_LR] = lr;
    c->r[REG_PC] = pc & ~1u;
    c->apsr = xpsr & 0xF8000000u;
    c->ipsr = xpsr & 0x1FFu;

    if (to_psp)        c->psp = sp + 32;
    else if (to_handler) c->msp = sp + 32;
    else               c->msp = sp + 32;

    if (to_handler) {
        c->mode = MODE_HANDLER;
        c->r[REG_SP] = c->msp;
    } else {
        c->mode = MODE_THREAD;
        /* Update CONTROL.SPSEL based on where we returned. */
        if (to_psp) {
            c->control |= 2u;
            c->r[REG_SP] = c->psp;
        } else {
            c->control &= ~2u;
            c->r[REG_SP] = c->msp;
        }
    }
    c->itstate = 0;
    return true;
}
