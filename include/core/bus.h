#ifndef CORTEX_M_BUS_H
#define CORTEX_M_BUS_H

#include "core/types.h"

/* Memory bus abstraction. Address space is divided into regions.
   Each region has either a flat backing buffer (RAM/Flash) or
   MMIO read/write callbacks (peripherals, NVIC, SCB). */

typedef enum {
    REGION_FLAT = 0,   /* r[offset], w[offset] direct */
    REGION_MMIO = 1,   /* callbacks invoked on access */
} region_kind_t;

typedef struct bus_s bus_t;

typedef u32 (*mmio_read_fn)(void* ctx, addr_t off, u32 size);
typedef void (*mmio_write_fn)(void* ctx, addr_t off, u32 val, u32 size);

typedef struct region_s {
    addr_t base;
    u32    size;
    region_kind_t kind;
    bool   writable;
    /* flat */
    u8*    buf;
    /* mmio */
    void*  ctx;
    mmio_read_fn  r;
    mmio_write_fn w;
    const char* name;
} region_t;

#define BUS_MAX_REGIONS 32

struct bus_s {
    region_t regs[BUS_MAX_REGIONS];
    u32   n;
    void* cookie;  /* per-run context; set by board_run to &run_ctx_t, cleared after */
};

void bus_init(bus_t* b);
int  bus_add_flat(bus_t* b, const char* name, addr_t base, u32 size, bool writable);
int  bus_add_mmio(bus_t* b, const char* name, addr_t base, u32 size,
                  void* ctx, mmio_read_fn r, mmio_write_fn w);

/* Read/write; size in bytes: 1, 2, 4. Returns false on fault. */
bool bus_read (bus_t* b, addr_t a, u32 size, u32* out);
bool bus_write(bus_t* b, addr_t a, u32 size, u32 val);

/* Fast helpers for aligned access on known-flat regions (hot path). */
u32  bus_r32(bus_t* b, addr_t a);
u16  bus_r16(bus_t* b, addr_t a);
u8   bus_r8 (bus_t* b, addr_t a);
void bus_w32(bus_t* b, addr_t a, u32 v);
void bus_w16(bus_t* b, addr_t a, u16 v);
void bus_w8 (bus_t* b, addr_t a, u8  v);

/* Load a firmware blob into a flat region at given address. */
bool bus_load_blob(bus_t* b, addr_t a, const u8* data, u32 n);

/* Find a REGION_FLAT region by exact base address; NULL if not found. */
region_t* bus_find_flat(bus_t* b, addr_t base);

/* Per-run cookie: set by board_run to run_ctx_t*; NULL after run completes.
   Peripherals read this to access per-board state without globals. */
static inline void  bus_set_cookie(bus_t* b, void* c) { b->cookie = c; }
static inline void* bus_get_cookie(const bus_t* b)    { return b->cookie; }

#endif
