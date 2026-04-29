#include "core/board.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static uint8_t* load_bin(const char* path, uint32_t* out_sz) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    (void)fread(buf, 1, (size_t)n, f);
    fclose(f);
    *out_sz = (uint32_t)n;
    return buf;
}

int main(int argc, char** argv) {
    const char* bin = argc > 1 ? argv[1] : "firmware/test1.bin";
    uint32_t sz = 0;
    uint8_t* fw = load_bin(bin, &sz);
    if (!fw) { fprintf(stderr, "SKIP: %s not found\n", bin); return 0; }

    board_t* a = board_create("generic-m4");
    board_t* b = board_create("generic-m4");
    if (!a || !b) { printf("FAIL create\n"); free(fw); return 1; }

    board_flash(a, fw, sz);
    board_flash(b, fw, sz);

    /* Enable TT on both boards */
    board_enable_timetravel(a, 5000, 100);
    board_enable_timetravel(b, 5000, 100);

    /* Inject IRQ only on A, verify B ev_log stays 0 */
    board_inject_irq(a, 16);
    uint32_t ev_a = board_get_ev_log_count(a);
    uint32_t ev_b = board_get_ev_log_count(b);
    printf("TT isolation: A=%u B=%u => %s\n", ev_a, ev_b,
           (ev_a == 1 && ev_b == 0) ? "PASS" : "FAIL");

    /* Run B for 10K steps, verify A's R0 not clobbered */
    uint32_t r0_a_before = board_cpu_reg(a, 0);
    int cause = 0;
    board_run(b, 10000, &cause);
    uint32_t r0_a_after = board_cpu_reg(a, 0);
    uint32_t r0_b       = board_cpu_reg(b, 0);

    printf("CPU isolation: A.R0=%u->%u B.R0=%u => %s\n",
           r0_a_before, r0_a_after, r0_b,
           (r0_a_before == r0_a_after) ? "PASS" : "FAIL");

    free(fw);
    board_destroy(a);
    board_destroy(b);

    int ok = (ev_a == 1 && ev_b == 0 && r0_a_before == r0_a_after);
    printf("lecerf-smoke: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
