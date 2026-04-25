#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/cpu.h"
#include "core/bus.h"
#include "core/decoder.h"
#include "periph/uart.h"
#include "periph/systick.h"
#include "periph/scb.h"
#include "periph/mpu.h"
#include "periph/stm32.h"
#include "periph/dwt.h"
#include "core/gdb.h"

extern dwt_t* g_dwt_for_run;

extern u64 run_steps_full_g(cpu_t* c, bus_t* bus, u64 max_steps,
                            systick_t* st, scb_t* scb, gdb_t* gdb);
extern u64 run_steps_full(cpu_t* c, bus_t* bus, u64 max_steps, systick_t* st, scb_t* scb);

/* Cortex-M memory layout defaults (ARM ARM B3):
   Flash   0x00000000 - 0x1FFFFFFF
   SRAM    0x20000000 - 0x3FFFFFFF
   Periph  0x40000000 - 0x5FFFFFFF  (MMIO — future)
*/
#define FLASH_BASE 0x00000000u
#define FLASH_SIZE (1u << 20)      /* 1 MB */
#define SRAM_BASE  0x20000000u
#define SRAM_SIZE  (256u << 10)    /* 256 KB */

static u8* read_file(const char* path, u32* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    u8* buf = (u8*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (u32)n;
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <firmware.bin> [max_steps] [--gdb=PORT]\n", argv[0]);
        return 1;
    }
    u64 max_steps = 10000000ull;
    int gdb_port = 0;
    for (int i = 2; i < argc; ++i) {
        if (strncmp(argv[i], "--gdb=", 6) == 0) gdb_port = atoi(argv[i] + 6);
        else max_steps = strtoull(argv[i], NULL, 0);
    }

    u32 sz = 0;
    u8* blob = read_file(argv[1], &sz);
    if (!blob) return 1;

    bus_t bus; bus_init(&bus);
    bus_add_flat(&bus, "flash", FLASH_BASE, FLASH_SIZE, false);
    bus_add_flat(&bus, "sram",  SRAM_BASE,  SRAM_SIZE,  true);
    static uart_t uart0 = {0};
    uart_attach(&bus, &uart0);
    static systick_t systick = {0};
    systick_attach(&bus, &systick);
    static scb_t scb = {0};
    scb_attach(&bus, &scb);
    static mpu_t mpu = {0};
    mpu_attach(&bus, &mpu);
    static stm32_t stm32;
    stm32_attach(&bus, &stm32);
    stm32.quiet = (gdb_port > 0);
    static dwt_t dwt = {0};
    dwt_attach(&bus, &dwt);
    g_dwt_for_run = &dwt;
    bus_load_blob(&bus, FLASH_BASE, blob, sz);
    free(blob);

    cpu_t cpu;
    cpu_reset(&cpu, CORE_M4);
    /* Cortex-M reset: SP = [0x0], PC = [0x4] per ARM ARM B1.5.5 */
    cpu.msp = bus_r32(&bus, 0x0);
    cpu.r[REG_SP] = cpu.msp;
    u32 entry = bus_r32(&bus, 0x4) & ~1u;
    cpu.r[REG_PC] = entry ? entry : FLASH_BASE;

    static gdb_t gdb = {0};
    gdb_t* g = NULL;
    if (gdb_port > 0) {
        if (gdb_listen(&gdb, gdb_port)) g = &gdb;
        else fprintf(stderr, "[gdb] failed to listen on :%d\n", gdb_port);
    }

    u64 n = run_steps_full_g(&cpu, &bus, max_steps, &systick, &scb, g);
    if (g) gdb_close(g);

    fprintf(stderr, "halted after %llu instructions\n", (unsigned long long)n);
    fprintf(stderr, "R0=%08x R1=%08x R2=%08x R3=%08x\n",
            cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3]);
    fprintf(stderr, "PC=%08x SP=%08x APSR=%08x\n",
            cpu.r[REG_PC], cpu.r[REG_SP], cpu.apsr);
    return 0;
}
