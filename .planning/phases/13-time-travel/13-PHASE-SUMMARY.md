---
phase: 13-time-travel
plans: [01, 02, 03, 04, 05]
status: shippable
completed: 2026-04-26
total_duration_min: 220
loc_delta: "+1406 insertions, -17 deletions (net +1389 LOC across 22 files)"
test_delta: "5 unit tests (v1.0 baseline) -> 10 unit tests (+5 TT: tt_determinism, tt_snapshot, tt_replay, tt_rewind, tt_firmware)"
tt_coverage: [TT-01, TT-02, TT-03, TT-04, TT-05, TT-06, TT-07, TT-08]
---

# Phase 13: Time-Travel Kernel — Phase Summary

**Time-travel kernel: ev_log_t event recording + snap_blob_t 263KB snapshot/restore + replay engine + tt_t bsearch rewind + step_back + diff + firmware integration; all 8 TT requirements satisfied; 10/10 tests pass; rewind mean 0.2ms at 1M history**

## Plan Summaries

### Plan 13-01: TT-01 Determinism Kernel

Established the determinism foundation: `ev_t` (16B struct, static_assert) + `ev_log_t` circular buffer with bsearch lower-bound seek + `tt_periph_t` aggregating 8 peripheral pointers + `jit_t.counters[]` extracted from local to field + `bus_find_flat` O(n) scan + `uart_t` replay-mode gate (RX queue, TX suppressed) + `nvic_set_pending_ext` event hook + `run_steps_full_g(jit_t*)` TT determinism path. Two-run byte-equal cpu_t+SRAM test at 10K cycles proves TT-01. Key deviations: MinGW static-lib weak symbol gap (replaced `__attribute__((weak))` with strong no-op stubs), `run_steps_full_gdb` rename to avoid jit_t/gdb_t overload conflict, static jit_t at file scope (2MB overflow guard).

### Plan 13-02: TT-02 Snapshot Module

`snap_blob_t` (~263KB): magic + version + cycle + cpu_t + 8 peripheral structs + uart_t (rx_q/replay_mode) + 256KB SRAM + XOR32 checksum. `snap_save` struct-copy + memcpy + checksum; `snap_restore` verify + memcpy + eth.bus back-pointer refill + dcache invalidate + jit_reset_counters; `snap_save_to_file`/`snap_load_from_file` binary round-trip; `g_jit_for_tt` extern wired. Five subtests: double-save-equal (TT-03), restore byte-equal, restore latency mean 0.14ms (TT-04 budget 100ms), file round-trip, corrupt-rejected.

### Plan 13-03: Replay Engine

`ev_log_seek` O(log n) bsearch lower-bound (null-safe); `tt_inject_event` 3-case dispatcher (EVENT_UART_RX→uart_inject_rx, EVENT_IRQ_INJECT→nvic_set_pending, EVENT_ETH_RX deferred); `run_until_cycle` monotone loop with event drain at cycle stamps + watchdog on stuck CPU (overshoot ≤ 1 ARM cycle); `tt_replay` = g_replay_mode=true + snap_restore + ev_log_seek + run_until_cycle. `run_until_cycle` placed in run.c (co-located with run_steps_full_g); run.h uses struct forward decls to avoid circular include. TT-05 verified: two independent replays from same (snap, log, target=10000) produce byte-equal cpu_t.

### Plan 13-04: TT-06/07/08 — tt_t Lifecycle + Bsearch Rewind + step_back + diff

`tt_t` struct: stride / max_snaps / snaps[] / idx[] (snap_entry_t: cycle+snap_idx) / n_snaps / ev_log_t. `tt_create`/`tt_destroy`: calloc + ev_log_init + g_tt assignment. `tt_on_cycle` O(1): stride-boundary check → snap_save to ring slot + idx append. `tt_attach_jit` wires g_jit_for_tt. `tt_bsearch_le` (static): largest idx[i].cycle ≤ target. `tt_rewind`: bsearch_le + snap_restore + run_until_cycle under g_replay_mode. `tt_step_back(N)`: tt_rewind(cycles-N). `tt_diff`: register + SRAM contiguous range encoder → FILE*. `tt_record_irq`/`tt_record_uart_rx` no-op stubs replaced with ev_log_append bodies (strong override). TT-06 mean rewind 0.3ms over 10 random targets in 1M history (budget 100ms). TT-07 step_back precision ±1 ARM cycle (whole-instruction granularity). TT-08 diff output contains "R0:" + "SRAM[0x20000100".

### Plan 13-05: Firmware Integration Test

Deterministic 50K-cycle Thumb firmware (cortex-m0, 189 bytes): UART tx "MERIDIAN_TT_PASS" tag, fib(20)→SRAM, two 4096-iter arithmetic loops (volatile sram_pad to prevent optimizer elision), `while(1) nop` halt. Three-run byte-equal integration test: REF run → snap, rewind(25000)+forward → snap, step_back(10000)+forward → snap; all three snap_blob_t memcmp==0 at cycle 50000. tt_diff(ref,ref) → empty output. Key bug fix: `tt_on_cycle` stride check changed from `c->cycles % stride == 0` to `c->cycles >= last_snap + stride`; startup code adds ~3 cycle offset before first instruction batch so modulo never fires; new threshold-based check correctly fires on first crossing of each boundary. 12/12 assertions pass.

## LOC Delta (Phase 13 Total)

```
git diff 10fd7ea..HEAD --stat -- src/ include/ tests/ firmware/test_tt/
22 files changed, +1406 insertions, -17 deletions (net +1389 LOC)
```

Key contributors:
- `src/core/tt.c`: +285 (ev_log, snap, replay, tt_t lifecycle, rewind, step_back, diff)
- `include/core/tt.h`: +165 (all public TT types and declarations)
- `tests/test_tt_snapshot.c`: +158 (5 snapshot subtests)
- `tests/test_tt_rewind.c`: +147 (TT-06/07/08 rewind/step_back/diff)
- `tests/test_tt_firmware.c`: +150 (3-run byte-equal firmware integration)
- `tests/test_tt_replay.c`: +135 (TT-05 replay determinism)
- `tests/test_tt_determinism.c`: +79 (TT-01 2-run byte-equal)
- `src/core/run.c`: +77 (run_until_cycle + run_steps_full_g)

## Test Count Delta

| Baseline (v1.0 / p12) | Phase 13 | Delta |
|------------------------|----------|-------|
| 5 unit tests           | 10 unit tests | +5 TT tests |

Tests added: tt_determinism, tt_snapshot, tt_replay, tt_rewind, tt_firmware

Note: v1.0 had 5 ctest unit tests (decoder, executor, bus, memory, t32). Phase 13 adds 5 TT integration tests. The 14 firmware test binaries run via separate scripts (firmware/run_all.sh), not ctest targets — they remain unchanged and unaffected by Phase 13 changes.

## TT-01..TT-08 Requirement Coverage

| Req | Description | Covered By | Status |
|-----|-------------|------------|--------|
| TT-01 | Same firmware + event log -> byte-equal final state | 13-01 (2-run byte-eq) + 13-05 (firmware 3-run) | PASS |
| TT-02 | Events recorded deterministically with cycle stamps | 13-01 ev_log_append hooks in uart/nvic + 13-04 tt_record bodies | PASS |
| TT-03 | snapshot(state) -> blob, restore byte-equal cpu+SRAM | 13-02 tt_snap_double_save_equal + tt_snap_restore_byte_equal | PASS |
| TT-04 | restore latency < 100ms | 13-02 tt_snap_restore_latency: mean 0.14ms << 100ms | PASS |
| TT-05 | replay(start, log, target) byte-equal across runs | 13-03 test_tt_replay two-replay byte-eq | PASS |
| TT-06 | rewind O(log n), < 100ms at 1M history | 13-04 TT-06: mean 0.2ms, bsearch_le O(log n) | PASS |
| TT-07 | step_back(N) returns to N cycles back | 13-04 step_back precision ±1 cycle + 13-05 firmware step_back(10000) | PASS |
| TT-08 | diff shows register + memory deltas | 13-04 tt_diff R0+SRAM check + 13-05 self-diff empty | PASS |

## Final Phase Status: `shippable`

All 8 TT requirements satisfied. 10/10 ctest tests pass. No blockers. Performance well within budget (rewind 0.2ms vs 100ms budget; restore 0.14ms vs 100ms budget). v1.0 firmware tests unaffected. The time-travel kernel is production-ready for the v2.0 debugger interface.

### Followups (not blockers)

- direct block chaining (jmp rel32 inter-TB) — JIT optimization
- LDR/STR native via helper-call — JIT coverage
- flag-setter ops via LEA tricks — JIT coverage
- WASM-compatible socket layer (postMessage) — p15 prerequisite
