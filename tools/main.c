#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <windows.h>
#endif

#include "core/board.h"
#include "core/gdb.h"

/* Legacy GDB path: keep the raw CPU/bus run_steps_full_gdb for --gdb= mode.
   Declared here; defined in src/core/run.c. */
extern u64 run_steps_full_gdb(cpu_t* c, bus_t* bus, u64 max_steps,
                              systick_t* st, scb_t* scb, gdb_t* gdb);

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
    int gdb_port  = 0;
    for (int i = 2; i < argc; ++i) {
        if (strncmp(argv[i], "--gdb=", 6) == 0) gdb_port = atoi(argv[i] + 6);
        else max_steps = strtoull(argv[i], NULL, 0);
    }

    u32 sz  = 0;
    u8* fw  = read_file(argv[1], &sz);
    if (!fw) return 1;

    /* --gdb path: bypass board API, run legacy raw pipeline */
    if (gdb_port > 0) {
        /* Re-use board_create just to get memory layout right, then poke
           internals.  We still need the raw CPU pointer for gdb_listen. */
        board_t* b = board_create("generic-m4");
        if (!b) { free(fw); return 1; }
        board_flash(b, fw, sz);
        free(fw);

        static gdb_t gdb = {0};
        gdb_t* g = NULL;
        if (gdb_listen(&gdb, gdb_port)) g = &gdb;
        else fprintf(stderr, "[gdb] failed to listen on :%d\n", gdb_port);

        u64 n = run_steps_full_gdb(&b->cpu, &b->bus, max_steps,
                                   &b->st, &b->scb, g);
        if (g) gdb_close(g);
        fprintf(stderr, "halted after %llu instructions\n", (unsigned long long)n);
        fprintf(stderr, "R0=%08x R1=%08x R2=%08x R3=%08x\n",
                b->cpu.r[0], b->cpu.r[1], b->cpu.r[2], b->cpu.r[3]);
        fprintf(stderr, "PC=%08x SP=%08x APSR=%08x\n",
                b->cpu.r[REG_PC], b->cpu.r[REG_SP], b->cpu.apsr);
        board_destroy(b);
        return 0;
    }

    /* Normal path: use board API */
    board_t* b = board_create("generic-m4");
    if (!b) { free(fw); fprintf(stderr, "board_create failed\n"); return 1; }

    if (!board_flash(b, fw, sz)) {
        free(fw);
        fprintf(stderr, "board_flash failed\n");
        board_destroy(b);
        return 1;
    }
    free(fw);

#ifdef _WIN32
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
#endif

    int exit_cause = 0;
    u64 n = board_run(b, max_steps, &exit_cause);

#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    double elapsed_s = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    double ips_m = (elapsed_s > 0.0) ? ((double)n / elapsed_s / 1e6) : 0.0;
#else
    double elapsed_s = 0.0;
    double ips_m     = 0.0;
#endif

    /* Drain UART TX to stdout */
    u8 uart_buf[4096];
    u32 uart_n = board_uart_drain(b, uart_buf, sizeof(uart_buf));
    if (uart_n > 0) fwrite(uart_buf, 1, uart_n, stdout);

    fprintf(stderr, "halted after %llu instructions\n", (unsigned long long)n);
#ifdef _WIN32
    fprintf(stderr, "IPS: %.2fM  elapsed: %.1fms\n", ips_m, elapsed_s * 1000.0);
#endif
    fprintf(stderr, "R0=%08x R1=%08x R2=%08x R3=%08x\n",
            board_cpu_reg(b, 0), board_cpu_reg(b, 1),
            board_cpu_reg(b, 2), board_cpu_reg(b, 3));
    fprintf(stderr, "PC=%08x SP=%08x APSR=%08x\n",
            board_cpu_reg(b, 15), board_cpu_reg(b, REG_SP), board_cpu_reg(b, 16));

    board_destroy(b);
    return 0;
}
