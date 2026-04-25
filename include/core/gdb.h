#ifndef CORTEX_M_GDB_H
#define CORTEX_M_GDB_H

#include "core/types.h"
#include "core/cpu.h"
#include "core/bus.h"

/* GDB Remote Serial Protocol over TCP. arm-none-eabi-gdb connects via
   `target remote :1234` and can step, read/write registers, set breakpoints. */

typedef struct gdb_s {
    int sock;          /* listening socket */
    int conn;          /* connected client */
    bool active;
    bool stepping;
    bool halted_for_gdb;
    /* Software breakpoints: list of PC addresses (max 32). */
    u32 bp[32];
    int bp_n;
} gdb_t;

bool gdb_listen(gdb_t* g, int port);
void gdb_close(gdb_t* g);
/* Called once per instruction; if a breakpoint hits or step done, returns true
   meaning the emulator should pause and process commands. */
bool gdb_should_stop(gdb_t* g, cpu_t* c);
/* Process incoming GDB packets while paused. Returns when GDB resumes. */
void gdb_serve(gdb_t* g, cpu_t* c, bus_t* b);

#endif
