# Roadmap: meridian v2.0

## Overview

5 phases (13-17). Phases 13-14 are the engine (time-travel + JIT depth).
Phases 15-17 are the three product faces (web learn / CI / distribution).

## Phases

- [ ] **Phase 13: Time-Travel Kernel** — determinism + snapshot + replay + rewind
- [ ] **Phase 14: JIT Depth** — direct chaining + LDR/STR native + flags via LEA
- [ ] **Phase 15: WASM + Web IDE** — emscripten port + Monaco editor + lessons
- [ ] **Phase 16: Python API + CI** — pytest plugin + GitHub Action + Docker
- [ ] **Phase 17: Landing & Distribution** — meridian.dev + release pipeline

## Phase Details

### Phase 13: Time-Travel Kernel

**Goal:** turn f(state, time, events) into a fully deterministic, snapshotable, reversible function.
This is the unique USP that beats QEMU/Renode.

**Depends on:** v1.0 (existing emulator).
**Requirements:** TT-01..TT-08
**Plans:** 5 plans

**Success criteria (observable):**
1. running same firmware twice with same event log produces byte-equal final state
2. snapshot(state) -> blob; restore(blob) reproduces register/memory exactly
3. rewind(N cycles back) seeks correctly across 1M+ cycle history under 100ms
4. step_back(1) returns to previous instruction in interactive debug
5. all 14 v1.0 firmware tests still pass

**Plans:**
- [ ] 13-01-PLAN.md — determinism kernel: ev_t/ev_log_t, jit_t.counters[], bus_find_flat, uart.replay_mode (Wave 1)
- [ ] 13-02-PLAN.md — snapshot module: snap_blob_t (~263KB) memcpy save/restore + xor32 + file rt (Wave 2)
- [ ] 13-03-PLAN.md — replay engine: ev_log_seek + tt_inject_event + run_until_cycle + tt_replay (Wave 2)
- [ ] 13-04-PLAN.md — rewind primitives: tt_t lifecycle + bsearch rewind + step_back + diff <100ms@1M (Wave 3)
- [ ] 13-05-PLAN.md — time-travel firmware test: fw_tt 50K cycles, 3-run byte-eq REF=rewind=stepback (Wave 4)

---

### Phase 14: JIT Depth

**Goal:** lift native JIT from 30M IPS to 100M+ IPS. Cover hot loops in real
firmware (FreeRTOS, DSP) without falling back to interpreter.

**Depends on:** Phase 13 (snapshot needed for safe TB invalidation).
**Requirements:** JIT-01..JIT-06

**Success criteria:**
1. native LDR/STR works for FreeRTOS context switch (no fallback)
2. CMP/ADDS/SUBS set NZCV flags correctly via x86 LEA
3. B.cond emits x86 jcc with rel32, falls through cleanly
4. direct block chaining: terminator jumps directly to next TB
5. FreeRTOS 5M instructions in under 50ms (100M+ IPS)
6. all v1.0 firmware tests pass through native JIT path

**Plans:**
- 14-01: helper-call ABI for memory ops, native LDR/STR
- 14-02: flag-setter ops (CMP family) via LEA + manual NZCV
- 14-03: B.cond + branch native emit
- 14-04: direct block chaining + LRU eviction
- 14-05: bench harness + speed regression test

---

### Phase 15: WASM + Web IDE

**Goal:** emulator runs in browser via WASM. Web IDE is the educational
front-end (meridian-learn product face).

**Depends on:** Phase 13 (clean determinism makes WASM port simpler).
**Requirements:** WEB-01..WEB-06

**Success criteria:**
1. emscripten produces .wasm + .js loader, page loads in <2s
2. firmware runs in browser, UART output visible in terminal pane
3. GPIO toggles drive on-page LED graphic
4. Monaco editor compiles C in browser via emcc.js (or pre-compiled lessons)
5. 5 lessons available, auto-grader pass/fail works
6. step / breakpoint / register inspect via web debug bridge

**Plans:**
- 15-01: emscripten build target + WASM-friendly socket abstraction
- 15-02: web debug bridge (postMessage replaces TCP RSP)
- 15-03: web IDE shell (React/Svelte) with editor + run + regs
- 15-04: 5 lessons + auto-grader
- 15-05: hosted demo on meridian.dev (or vercel preview)

---

### Phase 16: Python API + CI Runner

**Goal:** Python bindings, pytest plugin, Docker image, GitHub Action.
This is the meridian-ci product face.

**Depends on:** Phase 13 (snapshot/restore exposed in API).
**Requirements:** CI-01..CI-06

**Success criteria:**
1. pip install meridian (or local wheel) works
2. import meridian; Board("stm32f407").flash(...).run() returns exit cause
3. pytest can assert on UART output, register state, GPIO toggles
4. Docker image under 50MB, runs firmware test on stdin/stdout
5. GitHub Action sample repo passes a 3-test suite under 30s

**Plans:**
- 16-01: Python ctypes bindings or CFFI wrapper
- 16-02: pytest fixtures + assertions
- 16-03: Dockerfile + multi-stage minimal image
- 16-04: GitHub Action wrapper + sample workflow
- 16-05: example repo with 3-firmware test suite

---

### Phase 17: Landing & Distribution

**Goal:** meridian.dev live, release pipeline, docs.

**Depends on:** Phases 13-16 (need product to land).
**Requirements:** DIST-01..DIST-04

**Success criteria:**
1. meridian.dev landing page resolves with demo embed + install instructions
2. README.md has architecture diagram, ISA table, build/run, link to lessons
3. GitHub releases with single-binary tarballs for linux/macos/windows
4. CI workflow builds + tests + attaches release artifacts on tag push

**Plans:**
- 17-01: README rewrite v2 + diagrams
- 17-02: landing page (static, vercel/cloudflare)
- 17-03: release CI matrix (linux/macos/windows binaries)
- 17-04: tag v2.0.0 + first public release

## Progress

| Phase | Plans | Status |
|-------|-------|--------|
| 13. Time-Travel Kernel | 5 | Planned |
| 14. JIT Depth          | 5 | Not started |
| 15. WASM + Web IDE     | 5 | Not started |
| 16. Python API + CI    | 5 | Not started |
| 17. Landing & Dist     | 4 | Not started |

**Total:** 24 plans across 5 phases.

## Wave Plan: Phase 13

| Wave | Plans                | Depends on        | Notes                                                 |
|------|----------------------|-------------------|-------------------------------------------------------|
| 1    | 13-01                | (none)            | Determinism kernel: tt.h types, jit_t.cnt[], hooks    |
| 2    | 13-02, 13-03         | 13-01             | Parallel: snapshot + replay engine (no file overlap)  |
| 3    | 13-04                | 13-02, 13-03      | tt_t lifecycle + rewind/step_back/diff                |
| 4    | 13-05                | 13-04             | Integration firmware + 3-run byte-eq                  |
