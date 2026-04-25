#include "periph/dwt.h"

static u32 dwt_read(void* ctx, addr_t off, u32 size) {
    (void)size;
    dwt_t* d = (dwt_t*)ctx;
    if (off == 0x00) return d->ctrl;
    if (off == 0x04) return d->cyccnt;
    return 0;
}
static void dwt_write(void* ctx, addr_t off, u32 v, u32 size) {
    (void)size;
    dwt_t* d = (dwt_t*)ctx;
    if (off == 0x00) d->ctrl = v;
    else if (off == 0x04) d->cyccnt = v;
}
/* DEMCR alias (we don't fully model SCS region split) */
static u32 demcr_read(void* ctx, addr_t off, u32 size) {
    (void)size;
    dwt_t* d = (dwt_t*)ctx;
    if (off == 0x00) return d->demcr;
    return 0;
}
static void demcr_write(void* ctx, addr_t off, u32 v, u32 size) {
    (void)size;
    dwt_t* d = (dwt_t*)ctx;
    if (off == 0x00) d->demcr = v;
}

int dwt_attach(bus_t* b, dwt_t* d) {
    *d = (dwt_t){0};
    bus_add_mmio(b, "dwt",   DWT_BASE,    0x100, d, dwt_read, dwt_write);
    bus_add_mmio(b, "demcr", DEMCR_BASE,  0x004, d, demcr_read, demcr_write);
    return 0;
}

void dwt_tick(dwt_t* d) {
    if ((d->ctrl & 1u) && (d->demcr & (1u << 24))) d->cyccnt++;
}
