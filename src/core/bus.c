#include "core/bus.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void bus_init(bus_t* b) {
    memset(b, 0, sizeof(*b));
}

static region_t* find_region(bus_t* b, addr_t a) {
    for (u32 i = 0; i < b->n; ++i) {
        region_t* r = &b->regs[i];
        if (a >= r->base && a < r->base + r->size) return r;
    }
    return NULL;
}

int bus_add_flat(bus_t* b, const char* name, addr_t base, u32 size, bool writable) {
    if (b->n >= BUS_MAX_REGIONS) return -1;
    u8* buf = (u8*)calloc(1, size);
    if (!buf) return -1;
    region_t* r = &b->regs[b->n++];
    r->base = base; r->size = size;
    r->kind = REGION_FLAT; r->writable = writable;
    r->buf = buf; r->name = name;
    return 0;
}

int bus_add_mmio(bus_t* b, const char* name, addr_t base, u32 size,
                 void* ctx, mmio_read_fn rf, mmio_write_fn wf) {
    if (b->n >= BUS_MAX_REGIONS) return -1;
    region_t* r = &b->regs[b->n++];
    r->base = base; r->size = size;
    r->kind = REGION_MMIO; r->writable = true;
    r->ctx = ctx; r->r = rf; r->w = wf; r->name = name;
    return 0;
}

bool bus_read(bus_t* b, addr_t a, u32 size, u32* out) {
    region_t* r = find_region(b, a);
    if (!r) { fprintf(stderr, "[BUS FAULT] read addr=0x%08x size=%u: no region\n", a, size); return false; }
    u32 off = a - r->base;
    if (off + size > r->size) { fprintf(stderr, "[BUS FAULT] read addr=0x%08x size=%u: out of bounds (off=%u, rsize=%u)\n", a, size, off, r->size); return false; }
    if (r->kind == REGION_FLAT) {
        u32 v = 0;
        memcpy(&v, r->buf + off, size);
        *out = v;
        return true;
    }
    *out = r->r(r->ctx, off, size);
    return true;
}

bool bus_write(bus_t* b, addr_t a, u32 size, u32 val) {
    region_t* r = find_region(b, a);
    if (!r || !r->writable) { fprintf(stderr, "[BUS FAULT] write addr=0x%08x size=%u val=0x%08x: no region or not writable\n", a, size, val); return false; }
    u32 off = a - r->base;
    if (off + size > r->size) { fprintf(stderr, "[BUS FAULT] write addr=0x%08x size=%u val=0x%08x: out of bounds\n", a, size, val); return false; }
    if (r->kind == REGION_FLAT) {
        memcpy(r->buf + off, &val, size);
        return true;
    }
    r->w(r->ctx, off, val, size);
    return true;
}

u32 bus_r32(bus_t* b, addr_t a) { u32 v = 0; bus_read(b, a, 4, &v); return v; }
u16 bus_r16(bus_t* b, addr_t a) { u32 v = 0; bus_read(b, a, 2, &v); return (u16)v; }
u8  bus_r8 (bus_t* b, addr_t a) { u32 v = 0; bus_read(b, a, 1, &v); return (u8)v; }

void bus_w32(bus_t* b, addr_t a, u32 v) { bus_write(b, a, 4, v); }
void bus_w16(bus_t* b, addr_t a, u16 v) { bus_write(b, a, 2, v); }
void bus_w8 (bus_t* b, addr_t a, u8  v) { bus_write(b, a, 1, v); }

bool bus_load_blob(bus_t* b, addr_t a, const u8* data, u32 n) {
    region_t* r = find_region(b, a);
    if (!r || r->kind != REGION_FLAT) return false;
    u32 off = a - r->base;
    if (off + n > r->size) return false;
    memcpy(r->buf + off, data, n);
    return true;
}

region_t* bus_find_flat(bus_t* b, addr_t base) {
    for (u32 i = 0; i < b->n; ++i) {
        region_t* r = &b->regs[i];
        if (r->kind == REGION_FLAT && r->base == base) return r;
    }
    return NULL;
}
