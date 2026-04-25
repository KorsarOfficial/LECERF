# State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-26)

**Core value:** f(state, time, events) -> state' deterministic, fast, snapshotable, reversible
**Current focus:** v2.0 — time-travel + product platform

## Current Position

Phase: 13
Plan: 04 (next)
Status: 13-01+13-02+13-03 complete; TT-01 determinism + TT-03/04 snapshot + TT-05 replay done; ready for 13-04 tt_t/rewind
Last activity: 2026-04-26 — p13.03 TT-05 replay engine: ev_log_seek bsearch + tt_inject_event + run_until_cycle + tt_replay byte-eq

## Performance Metrics

v1.0 shipped: 12 phases, 62 tests, ~30M IPS hybrid native JIT.
p13.01: 3 tasks, 4 files created, 12 modified, 5->6 tests, 65 min.
p13.02: 1 task, 1 file created, 2 modified, 6->8 tests, ~20 min.
p13.03: 2 tasks, 1 file created, 6 modified, 8->8 tests (snapshot tests already counted), 45 min.

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

### Pending Todos

- direct block chaining (jmp rel32 inter-TB)
- LDR/STR native via helper-call
- flag-setter ops via LEA tricks
- WASM-compatible socket layer (postMessage)

### Blockers

none.

## Session Continuity

Last session: 2026-04-26
Stopped at: Completed 13-03-PLAN.md
Resume file: none
