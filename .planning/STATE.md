# State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-26)

**Core value:** f(state, time, events) -> state' deterministic, fast, snapshotable, reversible
**Current focus:** v2.0 — time-travel + product platform

## Current Position

Phase: 13
Plan: 06 (complete — TT-02 gap closed)
Status: ALL 13-01..13-06 complete; TT-01..TT-08 all verified including TT-02 ETH RX; 11/11 tests pass; 14/14 firmware tests pass
Last activity: 2026-04-26 — p13.06 TT-02 closure; eth_inject_rx + tt_record_eth_rx + side-blob store + EVENT_ETH_RX replay byte-eq

## Performance Metrics

v1.0 shipped: 12 phases, 62 tests, ~30M IPS hybrid native JIT.
p13.01: 3 tasks, 4 files created, 12 modified, 5->6 tests, 65 min.
p13.02: 2 tasks, 1 file created, 3 modified, 6->8 tests, 30 min.
p13.03: 2 tasks, 1 file created, 6 modified, 8->8 tests (snapshot tests already counted), 45 min.
p13.04: 2 tasks, 1 file created, 3 modified, 8->9 tests, 35 min.
p13.05: 2 tasks, 5 files created, 2 modified, 9->10 tests, 45 min.
p13.06: 2 tasks, 1 file created, 5 modified, 10->11 tests, 25 min.

## Accumulated Context

### Decisions

- p1..p12 all decisions logged in MILESTONES.md
- p12 native JIT pattern: rdi=cpu, [rdi+R_OFF+i*4] = r[i]; supports MOV/ADD/SUB/AND/OR/EOR + imm; fallback to interp on miss
- determinism kernel needed before snapshot (must remove implicit time/rand)
- WASM port deferred to p15 — needs WSA-free network stack first
- p13.01: tt_record_irq/uart_rx use strong no-ops (MinGW static-lib weak symbol gap); 13-04 replaces bodies
- p13.01: run_steps_full_g(jit_t*) is the TT determinism path; run_steps_full_gdb(gdb_t*) keeps gdb integration
- p13.01: jit_t is ~2MB; must not be stack-allocated (Windows 1MB default stack)
- p13.02: snap_blob_t includes uart_state (rx_q/replay_mode); eth.bus zeroed on save, refilled on restore
- p13.03: run_until_cycle in run.c (co-located with run_steps_full_g), declared in tt.h; run.h uses struct forward decls
- p13.03: snap_blob_t ~263KB; must be static in tests, same as jit_t ~2MB
- p13.04: tt_record_irq/uart_rx bodies replaced in-place (no weak override); targets in TT-06 test must be >= first snap cycle (stride boundary)
- p13.04: tt_diff uses FILE* -> stdio.h added to tt.h; snap_entry_t defined in tt.h alongside full tt_t struct
- p13.04: rewind mean latency 0.3ms at 1M history; bsearch 7 cmps + 0.14ms memcpy + worst-case 10K cycles forward replay
- p13.05: tt_on_cycle stride-boundary fix: use last_snap_cycle+stride threshold (not %stride==0); modulo fails when startup adds non-zero cycle offset before first instruction batch
- p13.05: firmware/test_tt flash at 0x00000000 (emulator default); volatile sram_pad needed to prevent optimizer from eliminating loops
- p13.05: phase 13 complete; TT-01..TT-08 all satisfied; 10/10 tests pass; shippable
- p13.06: TT-02 ETH gap closure; frames[] side-blob in tt_t (NOT snap_blob_t); ev_t.payload reused as u32 frame_id; eth_inject_rx dual-use (record-time + replay-time); EVENT_ETH_RX no longer a no-op; 11/11 tests pass

### Pending Todos

- direct block chaining (jmp rel32 inter-TB)
- LDR/STR native via helper-call
- flag-setter ops via LEA tricks
- WASM-compatible socket layer (postMessage)

### Blockers

none.

## Session Continuity

Last session: 2026-04-26
Stopped at: Completed 13-06-PLAN.md (TT-02 gap closed; phase 13 fully verified)
Resume file: none
