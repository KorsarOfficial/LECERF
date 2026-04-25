# State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-26)

**Core value:** f(state, time, events) -> state' deterministic, fast, snapshotable, reversible
**Current focus:** v2.0 — time-travel + product platform

## Current Position

Phase: 13 (next)
Plan: not started
Status: Defining v2.0 requirements
Last activity: 2026-04-26 — milestone v2.0 started after p12 (real x86 jit)

## Performance Metrics

v1.0 shipped: 12 phases, 62 tests, ~30M IPS hybrid native JIT.

## Accumulated Context

### Decisions

- p1..p12 all decisions logged in MILESTONES.md
- p12 native JIT pattern: rdi=cpu, [rdi+R_OFF+i*4] = r[i]; supports MOV/ADD/SUB/AND/OR/EOR + imm; fallback to interp on miss
- determinism kernel needed before snapshot (must remove implicit time/rand)
- WASM port deferred to p15 — needs WSA-free network stack first

### Pending Todos

- direct block chaining (jmp rel32 inter-TB)
- LDR/STR native via helper-call
- flag-setter ops via LEA tricks
- WASM-compatible socket layer (postMessage)

### Blockers

none.

## Session Continuity

Last session: 2026-04-26
Stopped at: end of p12 commit, milestone v2.0 starting
Resume file: none
